#include "movieplayer.hh"

#if PSXSPLASH_FEATURE_MOVIE

#if defined(LOADER_CDROM)

#include <new>

#include <psyqo-paths/str-player.hh>
#include <psyqo/alloc.h>
#include <psyqo/hardware/gpu.hh>
#include <psyqo/primitives/misc.hh>
#include <psyqo/xprintf.h>

#include "fileloader.hh"
#include "fileloader_cdrom.hh"

namespace {

using psyqo::paths::StrPlayer;

alignas(StrPlayer) uint8_t s_playerStorage[sizeof(StrPlayer)];
StrPlayer* s_player = nullptr;
StrPlayer::Options s_options;  // kept for a loop's next lap
uint32_t s_lba = 0, s_sectors = 0;

constexpr uint16_t c_chunkMagic = 0x0160;
constexpr uint32_t c_probeSectors = 16;  // a movie's first video sector is this close to its start

// StrPlayer's defaults are for 320x240: run-level buffers of 28672 halfwords, the
// most a retail movie that size has needed, and frames of up to 12 sectors. The
// budget a movie really needs comes off its first frame, which is what lets it
// play beside a big scene. A macroblock is six blocks of at most 65 codes, and an
// encoder working to a fixed bitrate does fill them on a small picture.
constexpr uint32_t c_rlPerMacroblock = 6 * 65;
constexpr uint8_t c_spareSectors = 2;
constexpr uint32_t c_chunkPayload = 2016;
constexpr uint32_t c_slotRecord = 32;   // StrPlayer::Slot is private; it is 28 bytes
constexpr uint32_t c_stackRoom = 16 * 1024;  // the heap runs to the top of RAM, where the stack lives

uint16_t le16(const uint8_t* p) { return p[0] | (p[1] << 8); }

// Sizes the player's buffers from the movie's first video sector, and hands back the picture's size.
void sizeBuffers(psyqo::CDRomDevice& cdrom, psyqo::GPU& gpu, uint32_t lba, uint32_t sectors, StrPlayer::Options& o,
                 uint16_t& pictureWidth, uint16_t& pictureHeight) {
    alignas(4) uint8_t sector[2048];
    for (uint32_t i = 0; i < c_probeSectors && i < sectors; i++) {
        if (!cdrom.readSectorsBlocking(lba + i, 1, sector, gpu)) return;
        if (le16(sector) != c_chunkMagic) continue;
        const uint32_t chunks = le16(sector + 6), width = le16(sector + 16), height = le16(sector + 18);
        const uint32_t macroblocks = ((width + 15) / 16) * ((height + 15) / 16);
        if (macroblocks == 0 || chunks == 0) return;
        pictureWidth = uint16_t(width);
        pictureHeight = uint16_t(height);
        const uint32_t rl = macroblocks * c_rlPerMacroblock;
        if (rl < o.rlHalfwords) o.rlHalfwords = rl;
        if (chunks + c_spareSectors < o.maxSectorsPerFrame) o.maxSectorsPerFrame = uint8_t(chunks + c_spareSectors);
        return;
    }
}

// StrPlayer cannot report what it says it reports: gcc takes `new` for never null,
// so its check after allocating is compiled out and a buffer that did not fit is
// written from address 0, over the exception vectors (forced with an impossible
// rlHalfwords, and the machine died). So the same blocks are tried here first,
// in its order, on an allocator that will give them back the same way.
bool fitsInHeap(const StrPlayer::Options& o) {
    const uint32_t slice = (o.rgb24 ? 24 : 16) * o.displayHeight * sizeof(uint16_t);
    const uint32_t run = o.rlHalfwords * sizeof(uint16_t);
    const uint32_t sizes[] = {o.frameSlots * c_slotRecord,
                              o.maxSectorsPerFrame * c_chunkPayload * o.frameSlots + 4,
                              run,
                              run,
                              slice,
                              slice,
                              c_stackRoom};
    void* blocks[sizeof(sizes) / sizeof(sizes[0])];
    bool fits = true;
    for (unsigned i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        blocks[i] = psyqo_malloc(sizes[i]);
        fits = fits && blocks[i];
    }
    for (void* block : blocks) {
        if (block) psyqo_free(block);
    }
    return fits;
}

// GP1(08h): 320 wide, as psyqo left it, at the movie's depth.
void setDisplayDepth(psyqo::GPU& gpu, bool rgb24) {
    psyqo::Hardware::GPU::Ctrl = 0x08000001u | (gpu.getRefreshRate() == 50 ? 0x08u : 0u) | (rgb24 ? 0x10u : 0u);
}

// GP0(80h), which psyqo has no primitive for.
struct VramCopy {
    uint32_t command = 0x80000000;
    psyqo::Vertex from, to, size;
};

// The shown frame into the other buffer, so a movie's window has the same picture
// around it on both.
void freezeFramebuffers(psyqo::GPU& gpu) {
    const int16_t back = gpu.getParity() ? 256 : 0;
    VramCopy copy;
    copy.from = {{.x = 0, .y = int16_t(256 - back)}};
    copy.to = {{.x = 0, .y = back}};
    copy.size = {{.w = 320, .h = 240}};
    gpu.sendPrimitive(copy);
    gpu.pumpCallbacks();
}

// Both framebuffers, so a movie smaller than the screen plays on black. 24-bit
// pixels take 480 VRAM columns for the 320 shown.
void clearFramebuffers(psyqo::GPU& gpu, bool rgb24) {
    psyqo::Prim::FastFill ff(psyqo::Color{.r = 0, .g = 0, .b = 0});
    const int16_t width = rgb24 ? 480 : 320;
    ff.rect = psyqo::Rect{0, 0, width, 240};
    gpu.sendPrimitive(ff);
    ff.rect = psyqo::Rect{0, 256, width, 240};
    gpu.sendPrimitive(ff);
    gpu.pumpCallbacks();
}

}  // namespace

bool psxsplash::MoviePlayer::request(const Request& request) {
    if (busy()) return false;
    m_request = request;
    m_pending = true;
    return true;
}

bool psxsplash::MoviePlayer::start(psyqo::GPU& gpu, Controls& pad) {
    m_gpu = &gpu;
    auto& loader = static_cast<FileLoaderCDRom&>(FileLoader::Get());
    psyqo::CDRomDevice* cdrom = loader.getCDRomDevice();

    char path[24];
    snprintf(path, sizeof path, "MOVIES/%s.STR;1", m_request.name);
    uint32_t lba = 0, size = 0;
    if (!loader.resolveBlocking(path, lba, size)) {
        printf("Movie %s: not on the disc\n", m_request.name);
        return false;
    }
    const uint32_t sectors = (size + 2047) / 2048;

    StrPlayer::Options options;
    options.rgb24 = m_request.rgb24;
    options.doubleSpeed = m_request.speed ? m_request.speed == 2 : FileLoaderCDRom::discDoubleSpeed();
    uint16_t width = 0, height = 0;
    sizeBuffers(*cdrom, gpu, lba, sectors, options, width, height);
    // The player centers the picture in the display size it is given, so a
    // display twice the corner plus the picture puts the picture at the corner.
    if (m_request.x >= 0 && width) options.displayWidth = uint16_t(2 * m_request.x + width);
    if (m_request.y >= 0 && height) options.displayHeight = uint16_t(2 * m_request.y + height);

    if (!fitsInHeap(options)) {
        printf("Movie %s: its buffers do not fit in the heap\n", m_request.name);
        return false;
    }

    if (m_request.rgb24) setDisplayDepth(gpu, true);
    if (m_request.overlay) {
        freezeFramebuffers(gpu);
    } else {
        clearFramebuffers(gpu, m_request.rgb24);
    }

    s_options = options;
    s_lba = lba;
    s_sectors = sectors;
    s_player = new (s_playerStorage) StrPlayer(*cdrom, gpu);
    s_player->play(lba, sectors, options);
    m_playing = true;
    m_skipHeld = skipPressed(pad);  // a press that started the movie does not end it
    return true;
}

bool psxsplash::MoviePlayer::skipPressed(Controls& pad) {
    return pad.isButtonHeld(psyqo::AdvancedPad::Button::Start) || pad.isButtonHeld(psyqo::AdvancedPad::Button::Cross);
}

bool psxsplash::MoviePlayer::frame(Controls& pad, Result& result) {
    const StrPlayer::Status status = s_player->frame();
    if (status == StrPlayer::Status::PLAYING) {
        const bool held = skipPressed(pad);
        const bool pressed = held && !m_skipHeld;
        m_skipHeld = held;
        if (!m_request.skippable || !pressed) return false;
        s_player->stop();
        result = Result::SKIPPED;
    } else {
        if (status == StrPlayer::Status::ENDED && m_request.loop && s_player->play(s_lba, s_sectors, s_options)) {
            return false;
        }
        result = status == StrPlayer::Status::ENDED ? Result::ENDED : Result::FAILED;
    }
    const auto& s = s_player->stats();
    printf("Movie %s: shown %u dropped %u rejected %u read errors %u bad addresses %u\n", m_request.name, s.framesShown,
           s.framesDropped, s.framesRejected, s.readErrors, s.badAddresses);
    finish();
    return true;
}

void psxsplash::MoviePlayer::finish() {
    if (s_player) {
        s_player->~StrPlayer();
        s_player = nullptr;
    }
    if (m_request.rgb24) {
        setDisplayDepth(*m_gpu, false);
        clearFramebuffers(*m_gpu, true);
    }
    m_playing = false;
}

#else  // PCdrv: there is no drive to stream from.

bool psxsplash::MoviePlayer::request(const Request&) { return false; }
bool psxsplash::MoviePlayer::start(psyqo::GPU&, Controls&) { return false; }
bool psxsplash::MoviePlayer::frame(Controls&, Result&) { return true; }
bool psxsplash::MoviePlayer::skipPressed(Controls&) { return false; }
void psxsplash::MoviePlayer::finish() {}

#endif  // LOADER_CDROM

#endif  // PSXSPLASH_FEATURE_MOVIE
