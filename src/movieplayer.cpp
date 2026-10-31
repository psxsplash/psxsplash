#include "movieplayer.hh"

#if PSXSPLASH_FEATURE_MOVIE

#if defined(LOADER_CDROM)

#include <new>

#include <psyqo-paths/str-player.hh>
#include <psyqo/hardware/gpu.hh>
#include <psyqo/primitives/misc.hh>
#include <psyqo/xprintf.h>

#include "fileloader.hh"
#include "fileloader_cdrom.hh"

namespace {

using psyqo::paths::StrPlayer;

alignas(StrPlayer) uint8_t s_playerStorage[sizeof(StrPlayer)];
StrPlayer* s_player = nullptr;

constexpr uint16_t c_chunkMagic = 0x0160;
constexpr uint32_t c_probeSectors = 16;  // a movie's first video sector is this close to its start

// StrPlayer's defaults are for 320x240: run-level buffers of 28672 halfwords, the
// most a retail movie that size has needed, and frames of up to 12 sectors. The
// budget a movie really needs comes off its first frame, which is what lets it
// play beside a big scene. A macroblock is six blocks of at most 65 codes, and an
// encoder working to a fixed bitrate does fill them on a small picture.
constexpr uint32_t c_rlPerMacroblock = 6 * 65;
constexpr uint8_t c_spareSectors = 2;

uint16_t le16(const uint8_t* p) { return p[0] | (p[1] << 8); }

// Sizes the player's buffers from the movie's first video sector.
void sizeBuffers(psyqo::CDRomDevice& cdrom, psyqo::GPU& gpu, uint32_t lba, uint32_t sectors, StrPlayer::Options& o) {
    alignas(4) uint8_t sector[2048];
    for (uint32_t i = 0; i < c_probeSectors && i < sectors; i++) {
        if (!cdrom.readSectorsBlocking(lba + i, 1, sector, gpu)) return;
        if (le16(sector) != c_chunkMagic) continue;
        const uint32_t chunks = le16(sector + 6), width = le16(sector + 16), height = le16(sector + 18);
        const uint32_t macroblocks = ((width + 15) / 16) * ((height + 15) / 16);
        if (macroblocks == 0 || chunks == 0) return;
        const uint32_t rl = macroblocks * c_rlPerMacroblock;
        if (rl < o.rlHalfwords) o.rlHalfwords = rl;
        if (chunks + c_spareSectors < o.maxSectorsPerFrame) o.maxSectorsPerFrame = uint8_t(chunks + c_spareSectors);
        return;
    }
}

// GP1(08h): 320 wide, as psyqo left it, at the movie's depth.
void setDisplayDepth(psyqo::GPU& gpu, bool rgb24) {
    psyqo::Hardware::GPU::Ctrl = 0x08000001u | (gpu.getRefreshRate() == 50 ? 0x08u : 0u) | (rgb24 ? 0x10u : 0u);
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
    sizeBuffers(*cdrom, gpu, lba, sectors, options);

    if (m_request.rgb24) setDisplayDepth(gpu, true);
    clearFramebuffers(gpu, m_request.rgb24);

    s_player = new (s_playerStorage) StrPlayer(*cdrom, gpu);
    if (!s_player->play(lba, sectors, options)) {
        printf("Movie %s: its buffers do not fit in the heap\n", m_request.name);
        finish();
        return false;
    }
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
        result = status == StrPlayer::Status::ENDED ? Result::ENDED : Result::FAILED;
    }
    const auto& s = s_player->stats();
    printf("Movie %s: shown %u dropped %u rejected %u read errors %u\n", m_request.name, s.framesShown, s.framesDropped,
           s.framesRejected, s.readErrors);
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
