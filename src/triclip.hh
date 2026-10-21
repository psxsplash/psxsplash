#pragma once

#include <stdint.h>
#include <psyqo/primitives/common.hh>

namespace psxsplash {

// Safe clamping bounds for PS1 GPU rasterizer limits (1023x511 max vertex delta).

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

// True if clampForRasterizer would move the vertex: one unsigned range test per axis.
inline bool outsideRasterizerRange(const psyqo::Vertex& v) {
    return ((uint16_t)(v.x - SAFE_MIN_X) > (uint16_t)(SAFE_MAX_X - SAFE_MIN_X)) |
           ((uint16_t)(v.y - SAFE_MIN_Y) > (uint16_t)(SAFE_MAX_Y - SAFE_MIN_Y));
}

// Clamp a projected triangle's three vertices; true if any of them moved,
// which bends the triangle on screen.
inline bool clampTriangleForRasterizer(psyqo::Vertex& v0, psyqo::Vertex& v1, psyqo::Vertex& v2) {
    if (!(outsideRasterizerRange(v0) | outsideRasterizerRange(v1) | outsideRasterizerRange(v2))) return false;
    clampForRasterizer(v0);
    clampForRasterizer(v1);
    clampForRasterizer(v2);
    return true;
}

}  // namespace psxsplash
