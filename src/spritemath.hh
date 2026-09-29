#pragma once

#include <stdint.h>

// The sprite data model and the arithmetic over it, deliberately free of psyqo,
// the GPU and every global. That is what lets tests/host/test_sprite.cpp compile
// this exact code natively and run it without a PlayStation.
//
// Everything that needs hardware lives in spritesystem.hh, which includes this.

namespace psxsplash {

/// A sprite sheet: one texture in the VRAM atlas, cut into a uniform cell grid.
///
/// Sheets are packed into the same atlas as UI images and 3D textures by the
/// exporter, so there is no per-sheet VRAM upload - by the time the runtime sees
/// a sheet, its pixels are already resident and these are just coordinates.
struct SpriteSheet {
    const char* name;  // points into splashpack data; fixed up by relocate()
    uint8_t texpageX, texpageY;
    uint16_t clutX, clutY;
    uint8_t u0, v0;  // texel of cell (0,0) within the texture page
    uint8_t cellW, cellH;
    uint8_t cols, rows;
    uint8_t bitDepth;  // 0 = 4bpp, 1 = 8bpp, 2 = 16bpp
};

/// A named animation: a run of consecutive cells within one sheet.
struct SpriteAnim {
    const char* name;  // points into splashpack data; fixed up by relocate()
    uint8_t sheet;
    uint8_t firstFrame;
    uint8_t frameCount;
    uint8_t frameDuration;  // vsync frames each animation frame is held
    bool loop;
};

namespace spritemath {

/// psyqo::Angle is FixedPoint<10> where 1.0 == 180 degrees, so a full turn is
/// 2048 raw units.
static constexpr int32_t c_angleFullTurn = 2048;

struct AnimTick {
    uint8_t frame;  // new index within the animation
    uint8_t timer;  // new dwell counter
    bool finished;  // a non-looping animation just reached its end
};

/// Advance an animation by exactly one vsync frame.
AnimTick advanceAnim(uint8_t frame, uint8_t timer, uint8_t frameCount, uint8_t frameDuration,
                     bool loop);

/// Absolute cell index -> texel coordinates within the sheet's page.
void frameToUV(const SpriteSheet& sheet, uint8_t frame, uint8_t& u, uint8_t& v);

/// Yaw -> direction index in [0, dirCount). Rounds to the nearest sector, so a
/// sprite facing exactly along a direction picks that direction.
uint8_t facingFromYaw(int32_t yawRaw, uint8_t dirCount);

}  // namespace spritemath
}  // namespace psxsplash
