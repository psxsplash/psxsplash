#pragma once

#include <stdint.h>
#include <psyqo/primitives/triangles.hh>
#include <psyqo/primitives/common.hh>

namespace psxsplash {

// ============================================================================
// Screen-space clipping types and functions
// ============================================================================

// Screen-space clip vertex with interpolatable attributes.
struct ClipVertex {
    int16_t x, y, z;
    uint8_t u, v;
    uint8_t r, g, b;
};

// Maximum output triangles from clipping a single triangle against 4 edges.
// Sutherland-Hodgman can produce up to 7 vertices -> 5 triangles in a fan.
static constexpr int MAX_CLIP_TRIS = 5;

// Result of screen-space triangle clipping.
struct ClipResult {
    ClipVertex verts[MAX_CLIP_TRIS * 3];
};

// GPU rasterizer limits: max vertex-to-vertex delta.
static constexpr int16_t MAX_DELTA_X = 1023;
static constexpr int16_t MAX_DELTA_Y = 511;

// Screen-space clip region. Must be narrower than rasterizer limits (1023x511)
// so that any triangle fully inside has safe vertex deltas.
// Centered on screen (160,120), extended to half the rasterizer max in each direction.
static constexpr int16_t CLIP_LEFT   = 160 - 510;   // -350
static constexpr int16_t CLIP_RIGHT  = 160 + 510;   //  670
static constexpr int16_t CLIP_TOP    = 120 - 254;    // -134
static constexpr int16_t CLIP_BOTTOM = 120 + 254;    //  374

// Check if all 3 vertices are on the same side of any screen edge -> invisible.
inline bool isCompletelyOutside(const psyqo::Vertex& v0,
                                const psyqo::Vertex& v1,
                                const psyqo::Vertex& v2) {
    int16_t x0 = v0.x, x1 = v1.x, x2 = v2.x;
    int16_t y0 = v0.y, y1 = v1.y, y2 = v2.y;

    if (x0 < CLIP_LEFT   && x1 < CLIP_LEFT   && x2 < CLIP_LEFT)   return true;
    if (x0 > CLIP_RIGHT  && x1 > CLIP_RIGHT  && x2 > CLIP_RIGHT)  return true;
    if (y0 < CLIP_TOP    && y1 < CLIP_TOP    && y2 < CLIP_TOP)    return true;
    if (y0 > CLIP_BOTTOM && y1 > CLIP_BOTTOM && y2 > CLIP_BOTTOM) return true;
    return false;
}

// Check if any vertex is outside the clip region or vertex deltas exceed
// rasterizer limits. If true, the triangle needs screen-space clipping.
inline bool needsClipping(const psyqo::Vertex& v0,
                          const psyqo::Vertex& v1,
                          const psyqo::Vertex& v2) {
    int16_t x0 = v0.x, x1 = v1.x, x2 = v2.x;
    int16_t y0 = v0.y, y1 = v1.y, y2 = v2.y;

    // Check if any vertex is outside the clip region.
    if (x0 < CLIP_LEFT || x0 > CLIP_RIGHT ||
        x1 < CLIP_LEFT || x1 > CLIP_RIGHT ||
        x2 < CLIP_LEFT || x2 > CLIP_RIGHT ||
        y0 < CLIP_TOP  || y0 > CLIP_BOTTOM ||
        y1 < CLIP_TOP  || y1 > CLIP_BOTTOM ||
        y2 < CLIP_TOP  || y2 > CLIP_BOTTOM) {
        return true;
    }

    // Check vertex-to-vertex deltas against rasterizer limits.
    int16_t minX = x0, maxX = x0;
    int16_t minY = y0, maxY = y0;
    if (x1 < minX) minX = x1; if (x1 > maxX) maxX = x1;
    if (x2 < minX) minX = x2; if (x2 > maxX) maxX = x2;
    if (y1 < minY) minY = y1; if (y1 > maxY) maxY = y1;
    if (y2 < minY) minY = y2; if (y2 > maxY) maxY = y2;

    if ((int32_t)(maxX - minX) > MAX_DELTA_X) return true;
    if ((int32_t)(maxY - minY) > MAX_DELTA_Y) return true;

    return false;
}

// Sutherland-Hodgman screen-space triangle clipping.
// Clips against CLIP_LEFT/RIGHT/TOP/BOTTOM, then triangulates the result.
// Returns number of output triangles (0 to MAX_CLIP_TRIS), vertices in result.
int clipTriangle(const ClipVertex& v0, const ClipVertex& v1, const ClipVertex& v2,
                 ClipResult& result);

// ============================================================================
// Near-plane (3D view-space) clipping types and functions
// ============================================================================

#define NEAR_PLANE_Z 48
#define MAX_NEARCLIP_TRIS 2

struct ViewVertex {
    int32_t x, y, z;
    uint8_t u, v;
    uint8_t r, g, b;
    uint8_t pad;
};

struct NearClipResult {
    int triCount;
    ViewVertex verts[MAX_NEARCLIP_TRIS * 3];
};

int nearPlaneClip(const ViewVertex& v0, const ViewVertex& v1, const ViewVertex& v2,
                  NearClipResult& result);
ViewVertex lerpViewVertex(const ViewVertex& a, const ViewVertex& b, int32_t t);

}  // namespace psxsplash
