#include "movietexture.hh"

#if PSXSPLASH_FEATURE_MOVIE

#if defined(LOADER_CDROM)

#include <psyqo/alloc.h>
#include <psyqo/xprintf.h>

#include <EASTL/algorithm.h>

extern "C" {
#include "bsdec/bsdec.h"
}
#include "common/hardware/dma.h"
#include "common/hardware/hwregs.h"

#include "fileloader.hh"
#include "fileloader_cdrom.hh"
#if PSXSPLASH_FEATURE_STREAMING
#include "streamreader.hh"
#endif

namespace {

volatile uint32_t& MDEC0 = *reinterpret_cast<volatile uint32_t*>(0x1f801820);
volatile uint32_t& MDEC1 = *reinterpret_cast<volatile uint32_t*>(0x1f801824);

constexpr uint32_t c_dmaSpin = 2000000;
constexpr uint32_t c_blocksPerMacroblock = 6;
constexpr uint32_t c_macroblock = 16;
constexpr uint16_t c_bsMarker = 0x3800;  // the second halfword of every BS frame
constexpr uint32_t c_opaqueBlack = 1u << 25;  // MDEC: set bit 15 of every pixel, or black would be see-through

// The tables StrPlayer seats, which every BS encoder assumes: Sony's standard
// quantization table in the zigzag order MDEC(2) takes, and the IDCT matrix.
constexpr uint8_t c_quant[64] = {
    2,  16, 16, 19, 16, 19, 22, 22, 22, 22, 22, 22, 26, 24, 26, 27, 27, 27, 26, 26, 26, 26,
    27, 27, 27, 29, 29, 29, 34, 34, 34, 29, 29, 29, 27, 27, 29, 29, 32, 32, 34, 34, 37, 38,
    37, 35, 35, 34, 35, 38, 38, 40, 40, 40, 48, 48, 46, 46, 56, 56, 58, 69, 69, 83,
};
constexpr int16_t c_scale[64] = {
    0x5a82,  0x5a82,  0x5a82,  0x5a82,  0x5a82,  0x5a82,  0x5a82,  0x5a82,  0x7d8a,  0x6a6d,  0x471c,
    0x18f8,  -0x18f9, -0x471d, -0x6a6e, -0x7d8b, 0x7641,  0x30fb,  -0x30fc, -0x7642, -0x7642, -0x30fc,
    0x30fb,  0x7641,  0x6a6d,  -0x18f9, -0x7d8b, -0x471d, 0x471c,  0x7d8a,  0x18f8,  -0x6a6e, 0x5a82,
    -0x5a83, -0x5a83, 0x5a82,  0x5a82,  -0x5a83, -0x5a83, 0x5a82,  0x471c,  -0x7d8b, 0x18f8,  0x6a6d,
    -0x6a6e, -0x18f9, 0x7d8a,  -0x471d, 0x30fb,  -0x7642, 0x7641,  -0x30fc, -0x30fc, 0x7641,  -0x7642,
    0x30fb,  0x18f8,  -0x471d, 0x6a6d,  -0x7d8b, 0x7d8a,  -0x6a6e, 0x471c,  -0x18f9,
};

uint32_t s_quantWords[32] __attribute__((aligned(4)));
uint32_t s_scaleWords[32] __attribute__((aligned(4)));

bool waitDma(int channel) {
    for (uint32_t i = 0; i < c_dmaSpin; i++) {
        if ((DMA_CTRL[channel].CHCR & 0x01000000) == 0) return true;
    }
    return false;
}

void sendTable(const uint32_t* words) {
    DMA_CTRL[DMA_MDECIN].MADR = reinterpret_cast<uintptr_t>(words);
    DMA_CTRL[DMA_MDECIN].BCR = 32 << 16 | 1;
    DMA_CTRL[DMA_MDECIN].CHCR = 0x01000201;
    waitDma(DMA_MDECIN);
}

void seatTables() {
    for (unsigned i = 0; i < 32; i++) {
        s_quantWords[i] = c_quant[(i * 4) & 63] | (c_quant[(i * 4 + 1) & 63] << 8) |
                          (c_quant[(i * 4 + 2) & 63] << 16) | (c_quant[(i * 4 + 3) & 63] << 24);
        s_scaleWords[i] = uint16_t(c_scale[i * 2]) | (uint32_t(uint16_t(c_scale[i * 2 + 1])) << 16);
    }
    DPCR |= 0x000000ff;
    MDEC1 = 0x80000000;
    MDEC1 = 0x60000000;
    MDEC0 = 0x40000001;
    sendTable(s_quantWords);
    MDEC0 = 0x60000000;
    sendTable(s_scaleWords);
}

uint16_t le16(const uint8_t* p) { return p[0] | (p[1] << 8); }

bool isFrameHeader(const uint8_t* p, uint16_t version) { return le16(p + 2) == c_bsMarker && le16(p + 6) == version; }

// The frames are all one size and the file does not say which: the size is the
// first distance from the start at which another frame header stands.
uint32_t frameStride(const uint8_t* file, uint32_t size) {
    const uint16_t version = le16(file + 6);
    for (uint32_t s = 64; s + 8 <= size; s += 4) {
        if (size % s == 0 && isFrameHeader(file + s, version)) return s;
    }
    return size;
}

uint32_t texelSpan(int lowest, int highest) { return uint32_t(highest - lowest + 8) & ~(c_macroblock - 1); }

}  // namespace

bool psxsplash::MovieTexture::play(const char* name, const GameObject* target, const Options& options) {
    stop();

    // The rectangle the target's triangles are mapped to. A mesh with more than
    // one texture gets the movie on the first one's triangles' rectangle.
    const Tri* first = nullptr;
    int minU = 255, minV = 255, maxU = 0, maxV = 0;
    for (int i = 0; i < target->polyCount; i++) {
        const Tri& t = target->polygons[i];
        if (t.isUntextured()) continue;
        if (!first) first = &t;
        if (t.tpage.getPageX() != first->tpage.getPageX() || t.tpage.getPageY() != first->tpage.getPageY()) continue;
        for (const auto& uv : {t.uvA, t.uvB}) {
            minU = eastl::min<int>(minU, uv.u);
            maxU = eastl::max<int>(maxU, uv.u);
            minV = eastl::min<int>(minV, uv.v);
            maxV = eastl::max<int>(maxV, uv.v);
        }
        minU = eastl::min<int>(minU, t.uvC.u);
        maxU = eastl::max<int>(maxU, t.uvC.u);
        minV = eastl::min<int>(minV, t.uvC.v);
        maxV = eastl::max<int>(maxV, t.uvC.v);
    }
    if (!first || first->tpage.getColorMode() != psyqo::Prim::TPageAttr::Tex16Bits) {
        printf("Movie %s: the target needs a 16-bit texture\n", name);
        return false;
    }
    const uint32_t width = texelSpan(minU, maxU), height = texelSpan(minV, maxV);
    if (!width || !height) {
        printf("Movie %s: the target's texture is smaller than a macroblock\n", name);
        return false;
    }

    auto& loader = static_cast<FileLoaderCDRom&>(FileLoader::Get());
    if (!loader.getCDRomDevice()->isIdle()) {
        printf("Movie %s: the drive is busy, start it before CD music\n", name);
        return false;
    }
#if PSXSPLASH_FEATURE_STREAMING
    if (StreamReader::Get().busy()) {
        printf("Movie %s: the drive is busy streaming\n", name);
        return false;
    }
#endif

    char path[24];
    snprintf(path, sizeof path, "MOVIES/%s.SBS;1", name);
    uint32_t lba = 0, bytes = 0;
    if (!loader.resolveBlocking(path, lba, bytes)) {
        printf("Movie %s: not on the disc\n", name);
        return false;
    }
    // The loader allocates with `new`, and nothing can see that fail (MoviePlayer).
    void* room = psyqo_malloc((bytes + 2047) & ~2047u);
    if (!room) {
        printf("Movie %s: the file does not fit in the heap\n", name);
        return false;
    }
    psyqo_free(room);
    int size = 0;
    uint8_t* file = loader.LoadFileSync(path, size);
    if (!file) {
        printf("Movie %s: not on the disc\n", name);
        return false;
    }
    if (size < 8 || le16(file + 2) != c_bsMarker) {
        printf("Movie %s: not a BS file\n", name);
        loader.FreeFile(file);
        return false;
    }

    const uint32_t stride = frameStride(file, size);
    const uint32_t frames = size / stride;
    uint32_t runLevels = 0;
    for (uint32_t i = 0; i < frames; i++) {
        runLevels = eastl::max(runLevels, bsdecRlHalfwords(file + i * stride, stride));
    }
    auto* rl = static_cast<uint16_t*>(psyqo_malloc(runLevels * sizeof(uint16_t)));
    auto* pixels = static_cast<uint16_t*>(psyqo_malloc(width * height * sizeof(uint16_t)));
    if (!runLevels || !rl || !pixels) {
        printf("Movie %s: its buffers do not fit in the heap\n", name);
        if (rl) psyqo_free(rl);
        if (pixels) psyqo_free(pixels);
        loader.FreeFile(file);
        return false;
    }

    m_file = file;
    m_runLevels = rl;
    m_pixels = pixels;
    m_runLevelHalfwords = runLevels;
    m_stride = stride;
    m_frames = frames;
    m_macroblocks = (width / c_macroblock) * (height / c_macroblock);
    m_x = int16_t(first->tpage.getPageX() * 64 + minU);
    m_y = int16_t(first->tpage.getPageY() * 256 + minV);
    m_width = int16_t(width);
    m_height = int16_t(height);
    m_fps = options.fps ? options.fps : 15;
    m_loop = options.loop;
    m_onComplete = options.onComplete;
    m_shown = 0xffffffff;
    m_startVsync = 0;
    snprintf(m_name, sizeof m_name, "%s", name);
    printf("Movie %s: %u frames of %u bytes, %ux%u at %d,%d\n", m_name, frames, stride, width, height, m_x, m_y);
    return true;
}

void psxsplash::MovieTexture::stop() {
    if (!m_file) return;
    FileLoader::Get().FreeFile(m_file);
    psyqo_free(m_runLevels);
    psyqo_free(m_pixels);
    m_file = nullptr;
    m_runLevels = m_pixels = nullptr;
}

bool psxsplash::MovieTexture::decode(uint32_t index, psyqo::GPU& gpu) {
    const BsdecResult r = bsdecFrame(m_file + index * m_stride, m_stride, m_runLevels, m_runLevelHalfwords);
    // At least: the padding behind a frame reads as a few more blocks (97 and 102
    // of 96 in a 64x64 file), and the MDEC never gets as far as those.
    if (!bsdecUsable(r.error) || r.blocks < m_macroblocks * c_blocksPerMacroblock) {
        printf("Movie %s: frame %u is smaller than %dx%d\n", m_name, index, m_width, m_height);
        return false;
    }

    seatTables();
    const uint32_t inWords = (r.halfwords + 1) / 2;
    const uint32_t outWords = uint32_t(m_width) * m_height / 2;
    MDEC0 = r.mdecCommand | c_opaqueBlack;
    // Both transfers are armed before either is waited on: the MDEC stops asking
    // for input once it holds a finished block.
    DMA_CTRL[DMA_MDECIN].MADR = reinterpret_cast<uintptr_t>(m_runLevels);
    DMA_CTRL[DMA_MDECIN].BCR = ((inWords + 31) / 32) << 16 | 32;
    DMA_CTRL[DMA_MDECIN].CHCR = 0x01000201;
    DMA_CTRL[DMA_MDECOUT].MADR = reinterpret_cast<uintptr_t>(m_pixels);
    DMA_CTRL[DMA_MDECOUT].BCR = (outWords / 32) << 16 | 32;
    DMA_CTRL[DMA_MDECOUT].CHCR = 0x01000200;
    const bool landed = waitDma(DMA_MDECOUT);
    // The picture is out, but the MDEC has not finished: the blocks a frame's
    // padding reads as are still going in, or waiting to come out. So this stops
    // it where it stands and the next frame starts from a reset.
    DMA_CTRL[DMA_MDECIN].CHCR = 0;
    if (!landed) {
        printf("Movie %s: the MDEC stopped at frame %u\n", m_name, index);
        DMA_CTRL[DMA_MDECOUT].CHCR = 0;
        return false;
    }

    // The MDEC's macroblocks run down the picture, so each column of them is one
    // contiguous 16-wide strip, which is one VRAM rectangle.
    gpu.waitChainIdle();
    for (int16_t x = 0; x < m_width; x += c_macroblock) {
        gpu.uploadToVRAM(m_pixels + x * m_height, psyqo::Rect{int16_t(m_x + x), m_y, c_macroblock, m_height});
    }
    return true;
}

bool psxsplash::MovieTexture::tick(psyqo::GPU& gpu) {
    if (!m_file) return false;
#if PSXSPLASH_FEATURE_STREAMING
    // A CD-ROM DMA does not complete beside an MDEC one (StrPlayer's finding on
    // hardware). The frame can wait; nothing starts a read until after this.
    if (StreamReader::Get().readInFlight()) return false;
#endif

    const uint32_t now = gpu.getLiveFrameCount();
    if (m_shown == 0xffffffff) m_startVsync = now;
    uint32_t index = (now - m_startVsync) * m_fps / gpu.getRefreshRate();
    if (index >= m_frames) {
        if (!m_loop) {
            printf("Movie %s: ended\n", m_name);
            stop();
            return true;
        }
        const uint32_t laps = index / m_frames;
        m_startVsync += laps * m_frames * gpu.getRefreshRate() / m_fps;
        index -= laps * m_frames;
    }
    if (index == m_shown) return false;
    m_shown = index;
    if (!decode(index, gpu)) stop();
    return false;
}

#else  // PCdrv: the file reads from the disc.

bool psxsplash::MovieTexture::play(const char*, const GameObject*, const Options&) { return false; }
void psxsplash::MovieTexture::stop() {}
bool psxsplash::MovieTexture::tick(psyqo::GPU&) { return false; }
bool psxsplash::MovieTexture::decode(uint32_t, psyqo::GPU&) { return false; }

#endif  // LOADER_CDROM

#endif  // PSXSPLASH_FEATURE_MOVIE
