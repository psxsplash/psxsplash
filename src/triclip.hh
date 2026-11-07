#pragma once

#include <stdint.h>
#include <psyqo/primitives/common.hh>

#include "screenmode.hh"

namespace psxsplash {

// Clamping bounds for the PS1 GPU rasterizer limits (1023x511 max vertex
// delta): safeBox() in screenmode.hh, centred on the current screen.

// Early-reject: all 3 vertices past the same screen edge.
inline bool isCompletelyOutside(const psyqo::Vertex& v0,
                                const psyqo::Vertex& v1,
                                const psyqo::Vertex& v2) {
    int16_t x0 = v0.x, x1 = v1.x, x2 = v2.x;
    int16_t y0 = v0.y, y1 = v1.y, y2 = v2.y;
    const SafeBox& b = safeBox();

    if (x0 < b.minX && x1 < b.minX && x2 < b.minX) return true;
    if (x0 > b.maxX && x1 > b.maxX && x2 > b.maxX) return true;
    if (y0 < b.minY && y1 < b.minY && y2 < b.minY) return true;
    if (y0 > b.maxY && y1 > b.maxY && y2 > b.maxY) return true;
    return false;
}

// Clamp a projected vertex to safe rasterizer range.
inline void clampForRasterizer(psyqo::Vertex& v) {
    const SafeBox& b = safeBox();
    if (v.x < b.minX) v.x = b.minX;
    else if (v.x > b.maxX) v.x = b.maxX;
    if (v.y < b.minY) v.y = b.minY;
    else if (v.y > b.maxY) v.y = b.maxY;
}

// True if clampForRasterizer would move the vertex: one unsigned range test per axis.
__attribute__((always_inline)) inline bool outsideRasterizerRange(const psyqo::Vertex& v) {
    const SafeBox& b = safeBox();
    return ((uint16_t)(v.x - b.minX) > (uint16_t)kSafeSpanX) | ((uint16_t)(v.y - b.minY) > (uint16_t)kSafeSpanY);
}

// True if the GTE may have saturated a vertex's projection (SX and SY are
// limited to -1024..1023), which can flip the triangle's winding on screen.
__attribute__((always_inline)) inline bool gteSaturated(const psyqo::Vertex& v) {
    return ((uint16_t)(v.x + 1023) > 2045) | ((uint16_t)(v.y + 1023) > 2045);
}
inline bool gteSaturated(const psyqo::Vertex& v0, const psyqo::Vertex& v1, const psyqo::Vertex& v2) {
    return gteSaturated(v0) | gteSaturated(v1) | gteSaturated(v2);
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
