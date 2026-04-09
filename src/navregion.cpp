#include "navregion.hh"

#include <psyqo/fixed-point.hh>
#include <psyqo/vector.hh>
#include <psyqo/xprintf.h>

/**
 * navregion.cpp - Convex Region Navigation System
 *
 * Key operations:
 *   - resolvePosition: O(1) typical (check current + neighbors via portals)
 *   - pointInRegion: O(n) per polygon vertices (convex cross test)
 *   - getFloorY: O(1) plane equation evaluation
 *   - findRegion: O(R) brute force, used only at init
 */

namespace psxsplash {

// ============================================================================
// Fixed-point helpers
// ============================================================================

static constexpr int FRAC_BITS = 12;
static constexpr int32_t FP_ONE = 1 << FRAC_BITS;

static inline int32_t fpmul(int32_t a, int32_t b) {
    return (int32_t)(((int64_t)a * b) >> FRAC_BITS);
}

static inline int32_t fpdiv(int32_t a, int32_t b) {
    if (b == 0) return 0;
    int32_t q = a / b;
    int32_t r = a - q * b;
    return q * FP_ONE + (r << FRAC_BITS) / b;
}

// ============================================================================
// Initialization
// ============================================================================

const uint8_t* NavRegionSystem::initializeFromData(const uint8_t* data) {
    const auto* hdr = reinterpret_cast<const NavDataHeader*>(data);
    m_header = *hdr;
    data += sizeof(NavDataHeader);

    m_regions = reinterpret_cast<const NavRegion*>(data);
    data += m_header.regionCount * sizeof(NavRegion);

    m_portals = reinterpret_cast<const NavPortal*>(data);
    data += m_header.portalCount * sizeof(NavPortal);



    return data;
}

// ============================================================================
// Point-in-convex-polygon (XZ plane)
// ============================================================================

bool NavRegionSystem::pointInConvexPoly(int32_t px, int32_t pz,
                                         const int32_t* vertsX, const int32_t* vertsZ,
                                         int vertCount) {
    if (vertCount < 3) return false;

    // For CCW winding, all cross products must be >= 0.
    // cross = (bx - ax) * (pz - az) - (bz - az) * (px - ax)
    for (int i = 0; i < vertCount; i++) {
        int next = (i + 1) % vertCount;
        int32_t ax = vertsX[i], az = vertsZ[i];
        int32_t bx = vertsX[next], bz = vertsZ[next];

        // Edge direction
        int32_t edgeX = bx - ax;
        int32_t edgeZ = bz - az;
        // Point relative to edge start
        int32_t relX = px - ax;
        int32_t relZ = pz - az;

        // Cross product (64-bit to prevent overflow)
        int64_t cross = (int64_t)edgeX * relZ - (int64_t)edgeZ * relX;
        if (cross < 0) return false;
    }
    return true;
}

// ============================================================================
// Closest point on segment (XZ only)
// ============================================================================

void NavRegionSystem::closestPointOnSegment(int32_t px, int32_t pz,
                                             int32_t ax, int32_t az,
                                             int32_t bx, int32_t bz,
                                             int32_t& outX, int32_t& outZ) {
    int32_t abx = bx - ax;
    int32_t abz = bz - az;
    int32_t lenSq = fpmul(abx, abx) + fpmul(abz, abz);
    if (lenSq == 0) {
        outX = ax; outZ = az;
        return;
    }

    int32_t dot = fpmul(px - ax, abx) + fpmul(pz - az, abz);
    // t = dot / lenSq, clamped to [0, 1]
    int32_t t;
    if (dot <= 0) {
        t = 0;
    } else if (dot >= lenSq) {
        t = FP_ONE;
    } else {
        t = fpdiv(dot, lenSq);
    }

    outX = ax + fpmul(t, abx);
    outZ = az + fpmul(t, abz);
}

// ============================================================================
// Get floor Y at position (plane equation)
// ============================================================================

int32_t NavRegionSystem::getFloorY(int32_t x, int32_t z, uint16_t regionIndex) const {
    if (regionIndex >= m_header.regionCount) return 0;
    const auto& reg = m_regions[regionIndex];

    // Y = planeA * X + planeB * Z + planeD
    // (all in 20.12, products need 64-bit intermediate)
    return fpmul(reg.planeA, x) + fpmul(reg.planeB, z) + reg.planeD;
}

// ============================================================================
// Point in region test
// ============================================================================

bool NavRegionSystem::pointInRegion(int32_t x, int32_t z, uint16_t regionIndex) const {
    if (regionIndex >= m_header.regionCount) return false;
    const auto& reg = m_regions[regionIndex];
    return pointInConvexPoly(x, z, reg.vertsX, reg.vertsZ, reg.vertCount);
}

// ============================================================================
// Find region (brute force, for initialization)
// ============================================================================

uint16_t NavRegionSystem::findRegion(int32_t x, int32_t z) const {
    // When multiple regions overlap at the same XZ position (e.g., floor and
    // elevated step), prefer the highest physical surface. In PSX Y-down space,
    // highest surface = smallest (most negative) floor Y value.
    uint16_t best = NAV_NO_REGION;
    int32_t bestY = 0x7FFFFFFF;
    for (uint16_t i = 0; i < m_header.regionCount; i++) {
        if (pointInRegion(x, z, i)) {
            int32_t fy = getFloorY(x, z, i);
            if (fy < bestY) {
                bestY = fy;
                best = i;
            }
        }
    }
    return best;
}

uint16_t NavRegionSystem::findRegionClosest(int32_t x, int32_t y, int32_t z) const {
    // When multiple regions overlap at the same XZ position (e.g., floor and
    // elevated step), prefer the closest physical surface to y
    
    uint16_t best = NAV_NO_REGION;
    int32_t shortestDistance = 0x7FFFFFFF;
    for (uint16_t i = 0; i < m_header.regionCount; i++) {
        if (pointInRegion(x, z, i)) {
            int32_t fy = getFloorY(x, z, i);

            int32_t distance = getYDistance(y,fy);
            if (distance < shortestDistance) {
                shortestDistance = distance;
                best = i;
            }
        }
    }
    if(best >= m_header.regionCount || shortestDistance > NAV_ATTACH_DISTANCE)
    {
        printf("OFF NAV Best is %d Distance: %d\n",best,shortestDistance);
        return NAV_NO_REGION;
    }
    
    printf("Returning Best Nav: %d\n",best);
    return best;
}

bool NavRegionSystem::isOffNavRegion(int32_t x, int32_t y, int32_t z) const {
    // When multiple regions overlap at the same XZ position (e.g., floor and
    // elevated step), prefer the closest physical surface to y
    
    uint16_t bestRegion = findRegionClosest(x,y,z);
    if(bestRegion == NAV_NO_REGION)
    {
        return true;
    }

    return false;
}

// ============================================================================
// Clamp position to region boundary
// ============================================================================

void NavRegionSystem::clampToRegion(int32_t& x, int32_t& z, uint16_t regionIndex) const {
    if (regionIndex >= m_header.regionCount) return;
    const auto& reg = m_regions[regionIndex];

    if (pointInConvexPoly(x, z, reg.vertsX, reg.vertsZ, reg.vertCount))
        return; // Already inside

    // Find closest point on any edge of the polygon
    int32_t bestX = x, bestZ = z;
    int64_t bestDistSq = 0x7FFFFFFFFFFFFFFFLL;

    for (int i = 0; i < reg.vertCount; i++) {
        int next = (i + 1) % reg.vertCount;
        int32_t cx, cz;
        closestPointOnSegment(x, z,
                              reg.vertsX[i], reg.vertsZ[i],
                              reg.vertsX[next], reg.vertsZ[next],
                              cx, cz);

        int64_t dx = (int64_t)(x - cx);
        int64_t dz = (int64_t)(z - cz);
        int64_t distSq = dx * dx + dz * dz;

        if (distSq < bestDistSq) {
            bestDistSq = distSq;
            bestX = cx;
            bestZ = cz;
        }
    }

    x = bestX;
    z = bestZ;
}

// ============================================================================
// Resolve position (main per-frame call)
// ============================================================================

int32_t NavRegionSystem::resolvePosition(int32_t& newX, int32_t& newY, int32_t& newZ,
                                          uint16_t& currentRegion) const {
    if (!isLoaded() || m_header.regionCount == 0) return 0;

    // If no valid region, find one
    if (currentRegion == NAV_NO_REGION || currentRegion >= m_header.regionCount) {
        currentRegion = findRegionClosest(newX, newY, newZ);
        printf("Newest Region %d \n",currentRegion);
        if (currentRegion == NAV_NO_REGION) return 0;
    }

    // Check if still in current region
    if (pointInRegion(newX, newZ, currentRegion)) {
        int32_t fy = getFloorY(newX, newZ, currentRegion);

        // Check if a portal neighbor has a higher floor at this position.
        // This handles overlapping regions (e.g., floor and elevated step).
        // When the player walks onto the step, the step region (portal neighbor)
        // has a higher floor (smaller Y in PSX Y-down) and should take priority.
        const auto& reg = m_regions[currentRegion];
        for (int i = 0; i < reg.portalCount; i++) {
            uint16_t portalIdx = reg.portalStart + i;
            if (portalIdx >= m_header.portalCount) break;
            uint16_t neighbor = m_portals[portalIdx].neighborRegion;
            if (neighbor >= m_header.regionCount) continue;
            if (pointInRegion(newX, newZ, neighbor)) {
                int32_t nfy = getFloorY(newX, newZ, neighbor);
                if (nfy < fy) {  // Higher physical surface (Y-down: smaller = higher)
                    currentRegion = neighbor;
                    fy = nfy;
                }
            }
        }

        return fy;
    }



    // Check portal neighbors
    const auto& reg = m_regions[currentRegion];
    for (int i = 0; i < reg.portalCount; i++) {
        uint16_t portalIdx = reg.portalStart + i;
        if (portalIdx >= m_header.portalCount) break;

        const auto& portal = m_portals[portalIdx];
        uint16_t neighbor = portal.neighborRegion;

        if (neighbor < m_header.regionCount && pointInRegion(newX, newZ, neighbor)) {
            currentRegion = neighbor;
            return getFloorY(newX, newZ, neighbor);
        }
    }

    /*
    // Not in current region or any neighbor — try broader search 
    // This handles jumping/falling to non-adjacent regions (e.g., landing on a platform) 
    { 
        uint16_t found = findRegionClosest(newX, newY, newZ); 
        if (found != NAV_NO_REGION) { 
            currentRegion = found; 
            return getFloorY(newX, newZ, found); 
        } 
    } 
    */
    //printf("Region is %d\n", currentRegion);
    // Truly off all regions — clamp to current region boundary
    //clampToRegion(newX, newZ, currentRegion);
    
    return getFloorY(newX, newZ, currentRegion);
}

// ============================================================================
// Pathfinding stub
// ============================================================================

bool NavRegionSystem::findPath(uint16_t startRegion, uint16_t endRegion,
                                NavPath& path) const {
    // STUB: Returns false until NPC pathfinding is implemented.
    path.stepCount = 0;
    (void)startRegion;
    (void)endRegion;
    return false;
}

// ============================================================================
// Get Y Distance (For region and player)
// ============================================================================

int32_t NavRegionSystem::getYDistance(int32_t firstY, int32_t secondY){
    // Abs
    firstY = firstY < 0 ? -firstY : firstY;
    secondY = secondY < 0 ? -secondY : secondY;

    // Difference
    return firstY < secondY ? secondY - firstY : firstY - secondY;
}


}  // namespace psxsplash
