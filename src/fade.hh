#pragma once

#include <stdint.h>

// Per-vertex fade toward the background (clear) colour, free of psyqo and the
// GTE so tests/host/test_fade.cpp compiles this exact code natively. Depths
// are GTE screen Z (SZ): view depth in raw 4.12 units, the same units as the
// ordering table index and the fog wall. 0 = vertex colour, 4096 = background.

namespace psxsplash::fade {

// Camera::ExtractFrustum's far plane: view depth 2000.0 in 4.12, i.e. this
// many SZ units. SZ saturates at 0xffff, so it never binds today; it is in
// the edge so that moving the plane in cannot bring back a hard cut.
constexpr int32_t kFarPlaneSZ = 4096 * 2000;
constexpr int32_t kMaxSZ = 0xffff;

// The fog ramp: 0 up to an eighth of the wall, linear to 4096 at the wall.
// fogFarSZ 0 = fog off.
inline int32_t fogIR(int32_t sz, int32_t fogFarSZ) {
    if (fogFarSZ <= 0) return 0;
    int32_t fogNear = fogFarSZ >> 3;
    int32_t range = fogFarSZ - fogNear;
    if (range < 1) range = 1;
    if (sz <= fogNear) return 0;
    if (sz >= fogFarSZ) return 4096;
    return ((sz - fogNear) * 4096) / range;
}

// The edge E: the nearest depth where something stops being drawn - the end of
// the ordering table (a triangle with every corner at or past it is dropped),
// the fog wall (every corner past it: culled), the frustum far plane, and the
// largest SZ the GTE gives. Once per frame.
inline int32_t edgeDepth(uint32_t otSize, int32_t fogFarSZ) {
    int32_t e = otSize < (uint32_t)kMaxSZ ? (int32_t)otSize : kMaxSZ;
    if (fogFarSZ > 0 && fogFarSZ < e) e = fogFarSZ;
    if (kFarPlaneSZ < e) e = kFarPlaneSZ;
    return e;
}

// The edge band: 0 before E - E/8, linear to 4096 at E. edge 0 = off.
inline int32_t edgeIR(int32_t sz, int32_t edge) {
    if (edge <= 0) return 0;
    int32_t start = edge - (edge >> 3);
    if (sz <= start) return 0;
    if (sz >= edge) return 4096;
    int32_t range = edge - start;
    if (range < 1) return 4096;
    return ((sz - start) * 4096) / range;
}

// What a vertex at depth sz fades by: the larger of the fog and the edge.
inline int32_t vertexIR(int32_t sz, int32_t fogFarSZ, int32_t edge) {
    int32_t f = fogIR(sz, fogFarSZ);
    if (f >= 4096 || edge <= 0) return f;
    int32_t e = edgeIR(sz, edge);
    return e > f ? e : f;
}

}  // namespace psxsplash::fade
