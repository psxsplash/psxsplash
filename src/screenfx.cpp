#include "screenfx.hh"

#include <psyqo/gte-registers.hh>
#include <psyqo/primitives/control.hh>
#include <psyqo/primitives/misc.hh>
#include <psyqo/primitives/rectangles.hh>
#include <psyqo/primitives/sprites.hh>

#include "common/hardware/counters.h"
#include "common/syscalls/syscalls.h"

using namespace psyqo::GTE;

namespace psxsplash {

ScreenFx ScreenFx::s_instance;

namespace {
// The fade's dither masks (screenfxmath.hh) and their CLUT, engine-owned VRAM
// in the font column the exporter keeps free, just above the system font
// (960, 464): rows 456-463. The editor's font packer stops at row 456.
constexpr int16_t kMaskX = 960, kMaskY = 456, kMaskClutX = 992;

// GP0(E2h) texture window: u = (u & ~(mask * 8)) | (offset & mask) * 8. A
// mask of 31 repeats one 8x8 tile, at (offsetX, offsetY) * 8 in the page.
struct TexWindow {
    uint32_t command;
    static constexpr uint32_t kOff = 0xe2000000;
    static uint32_t window(uint32_t offsetX, uint32_t offsetY) {
        return 0xe2000000 | 31 | (31 << 5) | ((offsetX & 31) << 10) | ((offsetY & 31) << 15);
    }
};

// The trail samples the previous frame in 64-pixel strips: one 15-bit texture
// page is 64 VRAM pixels wide per page step and a sprite's u stays in 0..63.
constexpr int16_t kStrip = 64;

#ifdef PSXSPLASH_PROFILER
struct Cost {
    Cost() { ScreenFx::get().costBegin(); }
    ~Cost() { ScreenFx::get().costEnd(); }
};
#define SCREENFX_COST() Cost cost_
#else
#define SCREENFX_COST()
#endif
}  // namespace

void ScreenFx::setScreen(int16_t width, int16_t height, bool doubleBuffered) {
    m_width = width;
    m_height = height;
    m_doubleBuffered = doubleBuffered;
#ifdef PSXSPLASH_PROFILER
    COUNTERS[2].mode = 0x200;  // system clock / 8, free running
#endif
}

void ScreenFx::reset() { m_state.reset(); }

void ScreenFx::uploadMasks(psyqo::GPU& gpu) {
    static uint16_t masks[screenfx::kMaskTexW / 4 * screenfx::kMaskTexH];
    screenfx::buildDitherMasks(masks);
    gpu.uploadToVRAM(masks, psyqo::Rect{.a = {.x = kMaskX, .y = kMaskY},
                                        .b = {screenfx::kMaskTexW / 4, screenfx::kMaskTexH}});
    // Entry 1: grey 16 of 31 with the mask bit, so (16 * colour) >> 7 is the
    // colour's own 5-bit value. Entry 0 stays 0: transparent.
    static uint16_t clut[16] = {0, 0x8000 | 16 | (16 << 5) | (16 << 10)};
    gpu.uploadToVRAM(clut, psyqo::Rect{.a = {.x = kMaskClutX, .y = kMaskY}, .b = {16, 1}});
}

void ScreenFx::tick(int32_t dt12) {
    SCREENFX_COST();
    m_state.tick(dt12);
}

void ScreenFx::trail(int32_t on) {
    if (on != 0 && !m_doubleBuffered) {
        if (!m_trailWarned) ramsyscall_printf("screenfx: trail needs double buffering, off in this display mode\n");
        m_trailWarned = true;
        return;
    }
    m_state.setTrail(on);
}

void ScreenFx::beginFrame() {
    SCREENFX_COST();
    int32_t dx = 0, dy = 0;
    m_state.shakeOffsets(dx, dy);
    // Renderer::Init set the centre; rewrite only while shaking and once after.
    const bool shaking = (dx | dy) != 0;
    if (!shaking && !m_offsetSet) return;
    m_offsetSet = shaking;
    write<Register::OFX, Unsafe>((uint32_t)((m_width / 2 + dx) << 16));
    write<Register::OFY, Unsafe>((uint32_t)((m_height / 2 + dy) << 16));
}

void ScreenFx::drawWorld(Renderer::OT& ot, Renderer::Balloc& balloc, unsigned parity, int32_t otSize) {
    SCREENFX_COST();
    if (m_state.idle() && !m_maskForced) return;
    const int32_t z = Renderer::SPRITE_DEPTH_BASE;
    m_trailNow = false;
    const int16_t bar = m_state.letterbox.value;
    if (bar > 0) {
        for (int i = 0; i < 2; i++) {
            auto& r = balloc.allocateFragment<psyqo::Prim::Rectangle>();
            r.primitive.setColor(psyqo::Color{.r = 0, .g = 0, .b = 0});
            r.primitive.position = {{.x = 0, .y = int16_t(i ? m_height - bar : 0)}};
            r.primitive.size = {{.x = m_width, .y = bar}};
            ot.insert(r, z);
        }
    }

    const bool trailOn = m_state.trail > 0 && m_doubleBuffered;
    // This frame draws into the buffer getNextClear clears; the other one is on
    // screen and holds the previous frame.
    m_prevPageY = parity == 0 ? 0 : 1;
    if (trailOn != m_maskForced) {
        // Semi-transparency applies only to texels with the mask bit set, and
        // the GPU sets it on what it draws only when told to. The first frame
        // with the trail on draws no strips: the previous frame lacks the bit.
        auto& mask = balloc.allocateFragment<psyqo::Prim::MaskControl>();
        mask.primitive = trailOn ? psyqo::Prim::MaskControl(psyqo::Prim::MaskControl::ForceSet)
                                 : psyqo::Prim::MaskControl(psyqo::Prim::MaskControl::FromSource);
        ot.insert(mask, otSize - 1);
        m_maskForced = trailOn;
        return;
    }
    // The strips go last in the overlay (drawOverlayPasses), after the fade
    // and the flash: the previous frame already carries them, so blending it
    // under them would apply them again every frame.
    m_trailNow = trailOn;
}

void ScreenFx::drawOverlay(psyqo::GPU& gpu, Renderer::Balloc& balloc) {
    drawOverlayPasses(gpu, balloc);
#ifdef PSXSPLASH_PROFILER
    // The overlay is the frame's last timed part (tick, beginFrame, drawWorld,
    // overlay): close the frame's total. avg and peak are both per frame.
    m_costTicks += m_costFrame;
    if (m_costFrame > m_costPeak) m_costPeak = m_costFrame;
    m_costFrame = 0;
    // Control: four empty sections timed the same way, the instrument's own cost.
    for (int i = 0; i < 4; i++) {
        Cost c;
    }
    m_ctrlTicks += m_costFrame;
    m_costFrame = 0;
    // Twice a second, outside the timed part: the effects' CPU time per frame
    // (counter 2 ticks are 8 system clocks, 0.236 us). The print itself takes
    // about a frame.
    if (++m_costFrames == 30) {
        const uint32_t avg = m_costTicks / 30;
        const uint32_t ctrl = m_ctrlTicks / 30;
        ramsyscall_printf("screenfx: cpu avg %u peak %u ticks (avg %u.%u us) ctrl %u ticks (%u.%u us) fade %d flash %d shake %d box %d trail %d\n",
                          avg, m_costPeak, avg * 236 / 1000, (avg * 236 / 100) % 10, ctrl, ctrl * 236 / 1000,
                          (ctrl * 236 / 100) % 10, m_state.fade.value, m_state.flash.value, m_state.shake.value,
                          m_state.letterbox.value, m_state.trail);
        m_costTicks = m_costFrames = m_costPeak = m_ctrlTicks = 0;
    }
#endif
}

void ScreenFx::drawOverlayPasses(psyqo::GPU& gpu, Renderer::Balloc& balloc) {
    SCREENFX_COST();
    if (m_state.idle()) return;
    auto pass = [&](psyqo::Prim::TPageAttr::SemiTrans mode, uint8_t r, uint8_t g, uint8_t b) {
        if ((r | g | b) == 0 && mode != psyqo::Prim::TPageAttr::HalfBackAndHalfFront) return;
        auto& tp = balloc.allocateFragment<psyqo::Prim::TPage>();
        tp.primitive.attr.set(mode).setDithering(true);
        gpu.chain(tp);
        auto& rect = balloc.allocateFragment<psyqo::Prim::Rectangle>();
        rect.primitive.setColor(psyqo::Color{.r = r, .g = g, .b = b});
        rect.primitive.setSemiTrans();
        rect.primitive.position = {{.x = 0, .y = 0}};
        rect.primitive.size = {{.x = m_width, .y = m_height}};
        gpu.chain(rect);
    };
    // Fade to a colour (see State::fadePasses): at 255 exactly the colour.
    const screenfx::State::FadePasses f = m_state.fadePasses();
    if (f.dither == screenfx::kDitherLevels) {
        pass(psyqo::Prim::TPageAttr::HalfBackAndHalfFront, m_state.fadeR, m_state.fadeG, m_state.fadeB);
    } else if (f.dither > 0) {
        // One full-screen sprite through dither tile f.dither - 1, repeated by
        // the texture window. Its on texels are mid grey with the mask bit
        // set, so the sprite colour gives the fade colour, blended 50/50; the
        // off texels are 0, which the GPU skips.
        auto& tp = balloc.allocateFragment<psyqo::Prim::TPage>();
        tp.primitive.attr.setPageX(kMaskX >> 6)
            .setPageY(kMaskY >> 8)
            .set(psyqo::Prim::TPageAttr::Tex4Bits)
            .set(psyqo::Prim::TPageAttr::HalfBackAndHalfFront)
            .setDithering(true);
        gpu.chain(tp);
        auto& win = balloc.allocateFragment<TexWindow>();
        win.primitive.command = TexWindow::window(f.dither - 1, (kMaskY & 255) >> 3);
        gpu.chain(win);
        auto& sp = balloc.allocateFragment<psyqo::Prim::Sprite>();
        sp.primitive.setColor(psyqo::Color{.r = m_state.fadeR, .g = m_state.fadeG, .b = m_state.fadeB});
        sp.primitive.setSemiTrans();
        sp.primitive.position = {{.x = 0, .y = 0}};
        sp.primitive.texInfo.u = 0;
        sp.primitive.texInfo.v = 0;
        sp.primitive.texInfo.clut = psyqo::PrimPieces::ClutIndex(kMaskClutX >> 4, kMaskY);
        sp.primitive.size = {{.x = m_width, .y = m_height}};
        gpu.chain(sp);
        auto& off = balloc.allocateFragment<TexWindow>();
        off.primitive.command = TexWindow::kOff;
        gpu.chain(off);
    }
    pass(psyqo::Prim::TPageAttr::FullBackSubFullFront, f.sub, f.sub, f.sub);
    pass(psyqo::Prim::TPageAttr::FullBackAndFullFront, f.addR, f.addG, f.addB);
    if (m_state.flash.value > 0) {
        uint8_t r, g, b;
        m_state.flashAdd(r, g, b);
        pass(psyqo::Prim::TPageAttr::FullBackAndFullFront, r, g, b);
    }
    // The trail: the previous frame, as shown (bars, UI, fade and flash
    // included), blended 50/50 over this one once it has all of them too.
    if (m_trailNow) {
        const uint8_t grey = screenfx::State::kTrailGrey;
        for (int16_t x = 0; x < m_width; x += kStrip) {
            auto& tp = balloc.allocateFragment<psyqo::Prim::TPage>();
            tp.primitive.attr.setPageX(uint8_t(x / kStrip))
                .setPageY(m_prevPageY)
                .set(psyqo::Prim::TPageAttr::Tex16Bits)
                .set(psyqo::Prim::TPageAttr::HalfBackAndHalfFront)
                .setDithering(true);
            gpu.chain(tp);
            auto& sp = balloc.allocateFragment<psyqo::Prim::Sprite>();
            sp.primitive.setColor(psyqo::Color{.r = grey, .g = grey, .b = grey});
            sp.primitive.setSemiTrans();
            sp.primitive.position = {{.x = x, .y = 0}};
            sp.primitive.texInfo.u = 0;
            sp.primitive.texInfo.v = 0;
            const int16_t w = m_width - x < kStrip ? int16_t(m_width - x) : kStrip;
            sp.primitive.size = {{.x = w, .y = m_height}};
            gpu.chain(sp);
        }
    }
}

#ifdef PSXSPLASH_PROFILER
void ScreenFx::costBegin() { m_costStart = (uint16_t)COUNTERS[2].value; }
void ScreenFx::costEnd() {
    m_costFrame += (uint16_t)((uint16_t)COUNTERS[2].value - m_costStart);
}
#endif

}  // namespace psxsplash
