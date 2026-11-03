#include "movieplayer.hh"

#if PSXSPLASH_FEATURE_MOVIE

#if defined(LOADER_CDROM)

#include <EASTL/algorithm.h>
#include <new>

#include <psyqo-paths/str-player.hh>
#include <psyqo/alloc.h>
#include <psyqo/hardware/gpu.hh>
#include <psyqo/primitives/misc.hh>
#include <psyqo/primitives/rectangles.hh>
#include <psyqo/xprintf.h>

#include "fileloader.hh"
#include "fileloader_cdrom.hh"
#include "scenemanager.hh"

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

static void reportSubtitle(void* name, unsigned number, psxsplash::srt::Problem problem) {
    if (problem == psxsplash::srt::Problem::LeftOut) {
        printf("Movie %s: subtitle %u is not a time line (00:00:01,500 --> 00:00:04,000) and its text, left out\n",
               static_cast<const char*>(name), number);
    } else {
        printf("Movie %s: subtitle %u has more than two rows of 40 characters, the rest is off screen\n",
               static_cast<const char*>(name), number);
    }
}

void psxsplash::MoviePlayer::loadSubtitles(FileLoaderCDRom& loader, Request& request) {
    char path[24];
    snprintf(path, sizeof path, "MOVIES/%s.SRT;1", request.name);
    int size = 0;
    uint8_t* file = loader.LoadFileSyncIfThere(path, size);
    if (!file) return;
    const char* text = reinterpret_cast<const char*>(file);
    const unsigned room = srt::count(text, size_t(size));
    if (room) request.subtitles = static_cast<Subtitle*>(psyqo_malloc(room * sizeof(Subtitle)));
    if (request.subtitles) {
        request.subtitleCount = uint16_t(srt::parse(text, size_t(size), request.subtitles, room, reportSubtitle, request.name));
    }
    FileLoader::Get().FreeFile(file);
    printf("Movie %s: %u subtitles\n", request.name, request.subtitleCount);
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

    if (!m_request.behind()) loadSubtitles(loader, m_request);
    if (m_request.subtitleCount) m_request.rgb24 = false;

    StrPlayer::Options options;
    options.rgb24 = m_request.rgb24;
    options.doubleSpeed = m_request.speed ? m_request.speed == 2 : FileLoaderCDRom::discDoubleSpeed();
    uint16_t width = 0, height = 0;
    sizeBuffers(*cdrom, gpu, lba, sectors, options, width, height);
    // The player centers the picture in the display size it is given, so a
    // display twice the corner plus the picture puts the picture at the corner.
    if (m_request.x >= 0 && width) options.displayWidth = uint16_t(2 * m_request.x + width);
    if (m_request.y >= 0 && height) options.displayHeight = uint16_t(2 * m_request.y + height);
    m_picX = int16_t((options.displayWidth - width) / 2);
    m_picY = int16_t((options.displayHeight - height) / 2);
    m_picW = int16_t(width);
    m_picH = int16_t(height);

    if (!fitsInHeap(options)) {
        printf("Movie %s: its buffers do not fit in the heap\n", m_request.name);
        releaseSubtitles();
        return false;
    }

    // A movie that follows another at the same depth and fills the screen keeps
    // the last one's picture up until its own first frame replaces it.
    const bool covers = width >= options.displayWidth && height >= options.displayHeight;
    const bool keep = m_held && !m_request.behind() && m_heldRgb24 == m_request.rgb24 && !m_request.overlay && covers;
    if (m_held && !keep) restoreDisplay();
    m_held = false;
    if (m_request.behind()) {
        if (width != m_request.vramWidth || height != m_request.vramHeight) {
            printf("Movie %s: %ux%u does not fill its image, %ux%u\n", m_request.name, width, height,
                   m_request.vramWidth, m_request.vramHeight);
            return false;
        }
        options.rgb24 = false;
        options.vramX = m_request.vramX;
        options.vramY = m_request.vramY;
    } else {
        if (m_request.rgb24) setDisplayDepth(gpu, true);
        if (m_request.overlay) {
            freezeFramebuffers(gpu);
        } else if (!keep) {
            clearFramebuffers(gpu, m_request.rgb24);
        }
    }

    s_options = options;
    s_lba = lba;
    s_sectors = sectors;
    s_player = new (s_playerStorage) StrPlayer(*cdrom, gpu);
    s_player->play(lba, sectors, options);
    m_playing = true;
    m_skipHeld = skipPressed(pad);  // a press that started the movie does not end it
    m_stopAsked = false;
    m_clockStarted = false;
    return true;
}

bool psxsplash::MoviePlayer::skipPressed(Controls& pad) {
    return pad.isButtonHeld(psyqo::AdvancedPad::Button::Start) || pad.isButtonHeld(psyqo::AdvancedPad::Button::Cross);
}

uint32_t psxsplash::MoviePlayer::framesShown() const { return s_player ? s_player->stats().framesShown : 0; }

bool psxsplash::MoviePlayer::frame(Controls& pad, Result& result) {
    // Behind a scene the last frame's chain may still be running, and the player's
    // uploads cannot share the GPU's DMA with it.
    if (m_request.behind()) m_gpu->waitChainIdle();
    // Movie.Stop: no frame more decodes, and the one up stays.
    const StrPlayer::Status status = m_stopAsked ? StrPlayer::Status::IDLE : s_player->frame();
    if (status == StrPlayer::Status::PLAYING) {
        if (!m_request.behind()) drawSubtitles();
        const bool held = skipPressed(pad);
        const bool pressed = held && !m_skipHeld;
        m_skipHeld = held;
        if (!m_request.skippable || !pressed) return false;
        s_player->stop();
        result = Result::SKIPPED;
    } else if (m_stopAsked) {
        s_player->stop();
        result = Result::STOPPED;
    } else {
        if (status == StrPlayer::Status::ENDED && m_request.loop && s_player->play(s_lba, s_sectors, s_options)) {
            return false;
        }
        result = status == StrPlayer::Status::ENDED ? Result::ENDED : Result::FAILED;
    }
    const auto& s = s_player->stats();
    m_lastShown = s.framesShown;
    m_lastDropped = s.framesDropped + s.framesRejected;
    printf("Movie %s: shown %u dropped %u rejected %u read errors %u bad addresses %u\n", m_request.name, s.framesShown,
           s.framesDropped, s.framesRejected, s.readErrors, s.badAddresses);
    finish();
    return true;
}

// The frame just decoded is in the back buffer and the flip has not happened, so
// text drawn here shows with its picture and goes with it.
void psxsplash::MoviePlayer::drawSubtitles() {
    if (!m_clockStarted) {
        m_clockStarted = true;
        m_clockStart = m_gpu->getLiveFrameCount();
    }
    if (!m_request.subtitleCount) return;
    psyqo::Font<>* font = SceneManager::GetFont();
    if (!font) return;
    // Nothing redraws a smaller picture's border, so the text drawn there last time would stay after its time:
    // the band the rows go in is blanked first, where the picture does not cover it.
    constexpr int16_t top = 200, bottom = 240;
    const int16_t px1 = int16_t(m_picX + m_picW), py1 = int16_t(m_picY + m_picH);
    auto blank = [this](int16_t x, int16_t y, int16_t w, int16_t h) {
        if (w <= 0 || h <= 0) return;
        psyqo::Prim::Rectangle r({.r = 0, .g = 0, .b = 0});
        r.position = {{.x = x, .y = y}};
        r.size = {{.x = w, .y = h}};
        m_gpu->sendPrimitive(r);
    };
    const int16_t a = eastl::max(top, m_picY), b = eastl::min(bottom, py1);
    if (b > a) {
        blank(0, a, m_picX, int16_t(b - a));
        blank(px1, a, int16_t(320 - px1), int16_t(b - a));
    }
    blank(0, top, 320, int16_t(eastl::min(m_picY, bottom) - top));
    blank(0, eastl::max(top, py1), 320, int16_t(bottom - eastl::max(top, py1)));
    const uint32_t vsyncs = m_gpu->getLiveFrameCount() - m_clockStart;
    const uint32_t ms = vsyncs * 1000 / m_gpu->getRefreshRate();
    for (unsigned i = 0; i < m_request.subtitleCount; i++) {
        const Subtitle& line = m_request.subtitles[i];
        if (ms < line.fromMs || ms >= line.toMs) continue;
        const char* second = line.text;
        while (*second && *second != '\n') second++;
        const size_t firstLength = second - line.text;
        const bool two = *second == '\n';
        if (two) second++;
        const size_t secondLength = two ? __builtin_strlen(second) : 0;
        const eastl::string_view rows[2] = {{line.text, firstLength}, {second, secondLength}};
        for (int row = 0; row < (two ? 2 : 1); row++) {
            const int16_t x = int16_t((320 - int(rows[row].size()) * 8) / 2);
            const int16_t y = int16_t((two ? 204 : 220) + row * 16);
            font->print(*m_gpu, rows[row], {{.x = int16_t(x + 1), .y = int16_t(y + 1)}}, {{.r = 0, .g = 0, .b = 0}});
            font->print(*m_gpu, rows[row], {{.x = x, .y = y}}, {{.r = 0xff, .g = 0xff, .b = 0xff}});
        }
    }
}

void psxsplash::MoviePlayer::releaseSubtitles() {
    if (m_request.subtitles) psyqo_free(m_request.subtitles);
    m_request.subtitles = nullptr;
    m_request.subtitleCount = 0;
}

void psxsplash::MoviePlayer::finish() {
    if (s_player) {
        s_player->~StrPlayer();
        s_player = nullptr;
    }
    m_held = !m_request.behind();
    m_heldRgb24 = m_request.rgb24;
    m_playing = false;
}

void psxsplash::MoviePlayer::restoreDisplay() {
    if (m_held && m_heldRgb24) {
        setDisplayDepth(*m_gpu, false);
        clearFramebuffers(*m_gpu, true);
    }
    m_held = false;
}

#else  // PCdrv: there is no drive to stream from.

bool psxsplash::MoviePlayer::request(const Request&) { return false; }
bool psxsplash::MoviePlayer::start(psyqo::GPU&, Controls&) { return false; }
bool psxsplash::MoviePlayer::frame(Controls&, Result&) { return true; }
bool psxsplash::MoviePlayer::skipPressed(Controls&) { return false; }
void psxsplash::MoviePlayer::finish() {}
uint32_t psxsplash::MoviePlayer::framesShown() const { return 0; }
void psxsplash::MoviePlayer::releaseSubtitles() {}
void psxsplash::MoviePlayer::restoreDisplay() {}

#endif  // LOADER_CDROM

#endif  // PSXSPLASH_FEATURE_MOVIE
