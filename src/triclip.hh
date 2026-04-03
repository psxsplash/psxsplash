#pragma once

#include <stdint.h>
#include <psyqo/primitives/common.hh>

namespace psxsplash {

// Safe clamping bounds for PS1 GPU rasterizer limits (1023×511 max vertex delta).

static constexpr int16_t SAFE_MIN_X = -351;
static constexpr int16_t SAFE_MAX_X =  672;
static constexpr int16_t SAFE_MIN_Y = -135;
static constexpr int16_t SAFE_MAX_Y =  376;

// Early-reject: all 3 vertices past the same screen edge.
inline bool isCompletelyOutside(const psyqo::Vertex& v0,
                                const psyqo::Vertex& v1,
                                const psyqo::Vertex& v2) {
    int16_t x0 = v0.x, x1 = v1.x, x2 = v2.x;
    int16_t y0 = v0.y, y1 = v1.y, y2 = v2.y;

    if (x0 < SAFE_MIN_X && x1 < SAFE_MIN_X && x2 < SAFE_MIN_X) return true;
    if (x0 > SAFE_MAX_X && x1 > SAFE_MAX_X && x2 > SAFE_MAX_X) return true;
    if (y0 < SAFE_MIN_Y && y1 < SAFE_MIN_Y && y2 < SAFE_MIN_Y) return true;
    if (y0 > SAFE_MAX_Y && y1 > SAFE_MAX_Y && y2 > SAFE_MAX_Y) return true;
    return false;
}

// Clamp a projected vertex to safe rasterizer range.
inline void clampForRasterizer(psyqo::Vertex& v) {
    if (v.x < SAFE_MIN_X) v.x = SAFE_MIN_X;
    else if (v.x > SAFE_MAX_X) v.x = SAFE_MAX_X;
    if (v.y < SAFE_MIN_Y) v.y = SAFE_MIN_Y;
    else if (v.y > SAFE_MAX_Y) v.y = SAFE_MAX_Y;
}

}  // namespace psxsplash
