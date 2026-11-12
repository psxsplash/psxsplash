#pragma once

#include <stdint.h>

// Nav region geometry free of psyqo, so that tests/host/test_navmath.cpp can
// compile this exact code natively. Coordinates are 20.12 world units.

namespace psxsplash {
namespace navmath {

/// How far, in portals and in regions, a clamp looks for the nearest boundary.
static constexpr int CLAMP_RINGS = 4;
static constexpr int CLAMP_REGIONS = 32;

/// True if (px, pz) is inside or on a CCW convex polygon.
inline bool pointInConvexPoly(int32_t px, int32_t pz, const int32_t* vertsX, const int32_t* vertsZ, int vertCount) {
    if (vertCount < 3) return false;
    for (int i = 0; i < vertCount; i++) {
        int next = (i + 1) % vertCount;
        int64_t cross = (int64_t)(vertsX[next] - vertsX[i]) * (pz - vertsZ[i]) -
                        (int64_t)(vertsZ[next] - vertsZ[i]) * (px - vertsX[i]);
        if (cross < 0) return false;
    }
    return true;
}

/// The point of segment ab closest to p (XZ). Products are 64-bit and the
/// division 32-bit (the engine links no 64-bit divide): t = dot / lenSq keeps
/// 16 fractional bits however short the edge. 4.12 products of raw positions
/// lost any edge shorter than about 64 units, and returned a vertex instead.
inline void closestPointOnSegment(int32_t px, int32_t pz, int32_t ax, int32_t az, int32_t bx, int32_t bz,
                                  int32_t& outX, int32_t& outZ) {
    int64_t abx = (int64_t)bx - ax, abz = (int64_t)bz - az;
    int64_t lenSq = abx * abx + abz * abz;
    int64_t dot = ((int64_t)px - ax) * abx + ((int64_t)pz - az) * abz;
    if (lenSq == 0 || dot <= 0) {
        outX = ax;
        outZ = az;
        return;
    }
    if (dot >= lenSq) {
        outX = bx;
        outZ = bz;
        return;
    }
    // 0 < dot < lenSq: scale both until dot << 16 fits in 31 bits.
    while (lenSq > (1 << 15)) {
        lenSq >>= 1;
        dot >>= 1;
    }
    int32_t t = (int32_t)(dot << 16) / (int32_t)lenSq;
    outX = ax + (int32_t)((abx * t + (1 << 15)) >> 16);
    outZ = az + (int32_t)((abz * t + (1 << 15)) >> 16);
}

/// The point of a region's boundary closest to (x, z), its squared distance,
/// and the edge it lies on.
template <class Region>
int64_t nearestBoundaryPoint(int32_t x, int32_t z, const Region& reg, int32_t& outX, int32_t& outZ, int& outEdge) {
    int64_t bestDistSq = 0x7FFFFFFFFFFFFFFFLL;
    outX = x;
    outZ = z;
    outEdge = -1;
    for (int i = 0; i < reg.vertCount; i++) {
        int next = (i + 1) % reg.vertCount;
        int32_t cx, cz;
        closestPointOnSegment(x, z, reg.vertsX[i], reg.vertsZ[i], reg.vertsX[next], reg.vertsZ[next], cx, cz);
        int64_t dx = (int64_t)x - cx, dz = (int64_t)z - cz;
        int64_t distSq = dx * dx + dz * dz;
        if (distSq < bestDistSq) {
            bestDistSq = distSq;
            outX = cx;
            outZ = cz;
            outEdge = i;
        }
    }
    return bestDistSq;
}

/// Clamps a position that is off the region to the nearest boundary point of
/// the region or of a region up to CLAMP_RINGS portals away. A wall usually
/// runs on across several regions, often a fan of them around one vertex;
/// clamping to the current region alone pinned a player sliding along such a
/// wall to the vertex where it leaves the current region. With selective set,
/// a nearest edge flagged walkoff leaves the position alone.
template <class Region, class Portal>
void clampNearby(int32_t& x, int32_t& z, uint16_t regionIndex, bool selective, const Region* regions,
                 uint16_t regionCount, const Portal* portals, uint16_t portalCount) {
    if (regionIndex >= regionCount) return;
    const Region& reg = regions[regionIndex];
    if (pointInConvexPoly(x, z, reg.vertsX, reg.vertsZ, reg.vertCount)) return;

    int32_t bestX, bestZ;
    int bestEdge;
    uint16_t bestRegion = regionIndex;
    int64_t bestDistSq = nearestBoundaryPoint(x, z, reg, bestX, bestZ, bestEdge);

    // Breadth-first over portals.
    uint16_t seen[CLAMP_REGIONS];
    int seenCount = 0, ringStart = 0;
    seen[seenCount++] = regionIndex;
    for (int ring = 0; ring < CLAMP_RINGS; ring++) {
        int ringEnd = seenCount;
        for (int k = ringStart; k < ringEnd; k++) {
            const Region& from = regions[seen[k]];
            for (int i = 0; i < from.portalCount; i++) {
                uint16_t portalIdx = from.portalStart + i;
                if (portalIdx >= portalCount) break;
                uint16_t neighbor = portals[portalIdx].neighborRegion;
                if (neighbor >= regionCount) continue;
                bool known = false;
                for (int s = 0; s < seenCount; s++) known |= seen[s] == neighbor;
                if (known || seenCount == CLAMP_REGIONS) continue;
                seen[seenCount++] = neighbor;

                // Skip a region whose bounding box is already farther than the best point.
                const Region& nr = regions[neighbor];
                int32_t minX = nr.vertsX[0], maxX = minX, minZ = nr.vertsZ[0], maxZ = minZ;
                for (int v = 1; v < nr.vertCount; v++) {
                    minX = nr.vertsX[v] < minX ? nr.vertsX[v] : minX;
                    maxX = nr.vertsX[v] > maxX ? nr.vertsX[v] : maxX;
                    minZ = nr.vertsZ[v] < minZ ? nr.vertsZ[v] : minZ;
                    maxZ = nr.vertsZ[v] > maxZ ? nr.vertsZ[v] : maxZ;
                }
                int64_t ox = x < minX ? (int64_t)minX - x : x > maxX ? (int64_t)x - maxX : 0;
                int64_t oz = z < minZ ? (int64_t)minZ - z : z > maxZ ? (int64_t)z - maxZ : 0;
                if (ox * ox + oz * oz >= bestDistSq) continue;

                int32_t cx, cz;
                int edge;
                int64_t distSq = nearestBoundaryPoint(x, z, nr, cx, cz, edge);
                if (distSq < bestDistSq) {
                    bestDistSq = distSq;
                    bestX = cx;
                    bestZ = cz;
                    bestEdge = edge;
                    bestRegion = neighbor;
                }
            }
        }
        ringStart = ringEnd;
    }

    if (selective && bestEdge >= 0 && (regions[bestRegion].walkoffEdgeMask & (1 << bestEdge))) return;
    x = bestX;
    z = bestZ;
}

}  // namespace navmath
}  // namespace psxsplash
