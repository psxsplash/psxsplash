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
    // Prefer the highest surface (smallest Y in Y-down space)
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
    // Prefer the closest surface to y, skipping regions the player is below
    
    uint16_t best = NAV_NO_REGION;
    int32_t shortestDistance = 0x7FFFFFFF;
    for (uint16_t i = 0; i < m_header.regionCount; i++) {
        if (pointInRegion(x, z, i)) {
            int32_t fy = getFloorY(x, z, i);

            // Player below the region so skip
            if(y-32 > fy){
                continue;
            }
            int32_t distance = getYDistance(y,fy);
            if (distance < shortestDistance) {
                shortestDistance = distance;
                best = i;
            }
        }
    }
    if(best < m_header.regionCount && shortestDistance <= NAV_ATTACH_DISTANCE)
    {
        return best;
    }

    // Fallback: actor is outside all region polygons (e.g. spawned at edge).
    // Return the nearest region by centroid XZ distance so agents can still path.
    uint16_t nearest = NAV_NO_REGION;
    int32_t nearestDist = 0x7FFFFFFF;
    for (uint16_t i = 0; i < m_header.regionCount; i++) {
        int32_t cx = 0, cy = 0, cz = 0;
        if (!getRegionCenter(i, cx, cy, cz)) continue;
        int32_t dx = cx - x; if (dx < 0) dx = -dx;
        int32_t dz = cz - z; if (dz < 0) dz = -dz;
        int32_t dist = dx > dz ? dx : dz;
        if (dist < nearestDist) {
            nearestDist = dist;
            nearest = i;
        }
    }
    return nearest;
}

bool NavRegionSystem::getRegionCenter(uint16_t regionIndex, int32_t& outX, int32_t& outY, int32_t& outZ) const {
    if (regionIndex >= m_header.regionCount || m_regions == nullptr) return false;

    const auto& reg = m_regions[regionIndex];
    if (reg.vertCount < 3) return false;

    int32_t sumX = 0;
    int32_t sumZ = 0;
    for (int i = 0; i < reg.vertCount; ++i) {
        sumX += reg.vertsX[i];
        sumZ += reg.vertsZ[i];
    }

    outX = sumX / reg.vertCount;
    outZ = sumZ / reg.vertCount;
    outY = getFloorY(outX, outZ, regionIndex);
    return true;
}

bool NavRegionSystem::isOffNavRegion(int32_t x, int32_t y, int32_t z) const {
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
// Clamp position to non-walkoff boundary edges only
// ============================================================================

void NavRegionSystem::clampToRegionSelective(int32_t& x, int32_t& z, uint16_t regionIndex) const {
    if (regionIndex >= m_header.regionCount) return;
    const auto& reg = m_regions[regionIndex];

    if (pointInConvexPoly(x, z, reg.vertsX, reg.vertsZ, reg.vertCount))
        return; // Already inside

    // Find which edge the player is closest to.
    // If that edge is a walkoff edge, let them leave without clamping.
    int32_t bestX = x, bestZ = z;
    int64_t bestDistSq = 0x7FFFFFFFFFFFFFFFLL;
    int bestEdge = -1;

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
            bestEdge = i;
        }
    }

    // If the closest edge allows walkoff, do not clamp
    if (bestEdge >= 0 && (reg.walkoffEdgeMask & (1 << bestEdge))) {
        return;
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
        if (currentRegion == NAV_NO_REGION) return 0;
    }

    // Check if still in current region
    if (pointInRegion(newX, newZ, currentRegion)) {
        int32_t fy = getFloorY(newX, newZ, currentRegion);

        // Prefer portal neighbor with a higher floor (smaller Y in Y-down space)
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
    // Not in current region or any neighbor -- try broader search 
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
    // Off all regions -- clamp to current region boundary
    clampToRegion(newX, newZ, currentRegion);
    
    return getFloorY(newX, newZ, currentRegion);
}

// ============================================================================
// Pathfinding stub
// ============================================================================

bool NavRegionSystem::findPath(uint16_t startRegion, uint16_t endRegion,
                                NavPath& path) const {
    path.stepCount = 0;
    if (!isLoaded() || m_regions == nullptr) return false;
    if (startRegion >= m_header.regionCount || endRegion >= m_header.regionCount) return false;

    if (startRegion == endRegion) {
        path.regions[0] = startRegion;
        path.stepCount = 1;
        return true;
    }

    if (m_header.regionCount > NAV_MAX_SEARCH_REGIONS) {
        return false;
    }

    static constexpr int32_t kInfiniteCost = 0x3FFFFFFF;

    uint8_t openSet[NAV_MAX_SEARCH_REGIONS] = {};
    uint8_t closedSet[NAV_MAX_SEARCH_REGIONS] = {};
    uint16_t cameFrom[NAV_MAX_SEARCH_REGIONS];
    int32_t gScore[NAV_MAX_SEARCH_REGIONS];
    int32_t fScore[NAV_MAX_SEARCH_REGIONS];

    for (uint16_t i = 0; i < m_header.regionCount; ++i) {
        cameFrom[i] = NAV_NO_REGION;
        gScore[i] = kInfiniteCost;
        fScore[i] = kInfiniteCost;
    }

    auto regionHeuristic = [this](uint16_t fromRegion, uint16_t toRegion) -> int32_t {
        int32_t fromX, fromY, fromZ;
        int32_t toX, toY, toZ;
        if (!getRegionCenter(fromRegion, fromX, fromY, fromZ)) return 0;
        if (!getRegionCenter(toRegion, toX, toY, toZ)) return 0;

        int32_t dx = fromX - toX;
        int32_t dz = fromZ - toZ;
        if (dx < 0) dx = -dx;
        if (dz < 0) dz = -dz;
        return dx + dz;
    };

    auto edgeCost = [this](uint16_t fromRegion, uint16_t toRegion) -> int32_t {
        int32_t fromX, fromY, fromZ;
        int32_t toX, toY, toZ;
        if (!getRegionCenter(fromRegion, fromX, fromY, fromZ)) return 1;
        if (!getRegionCenter(toRegion, toX, toY, toZ)) return 1;

        int32_t dx = fromX - toX;
        int32_t dz = fromZ - toZ;
        if (dx < 0) dx = -dx;
        if (dz < 0) dz = -dz;
        int32_t cost = dx + dz;
        return cost > 0 ? cost : 1;
    };

    openSet[startRegion] = 1;
    gScore[startRegion] = 0;
    fScore[startRegion] = regionHeuristic(startRegion, endRegion);

    while (true) {
        uint16_t current = NAV_NO_REGION;
        int32_t bestScore = kInfiniteCost;

        for (uint16_t i = 0; i < m_header.regionCount; ++i) {
            if (!openSet[i]) continue;
            if (fScore[i] < bestScore) {
                bestScore = fScore[i];
                current = i;
            }
        }

        if (current == NAV_NO_REGION) {
            return false;
        }

        if (current == endRegion) {
            uint16_t reversePath[NAV_MAX_PATH_STEPS];
            int count = 0;
            uint16_t walk = endRegion;
            while (walk != NAV_NO_REGION && count < NAV_MAX_PATH_STEPS) {
                reversePath[count++] = walk;
                if (walk == startRegion) break;
                walk = cameFrom[walk];
            }

            if (count == 0 || reversePath[count - 1] != startRegion) {
                path.stepCount = 0;
                return false;
            }

            path.stepCount = count;
            for (int i = 0; i < count; ++i) {
                path.regions[i] = reversePath[count - 1 - i];
            }
            return true;
        }

        openSet[current] = 0;
        closedSet[current] = 1;

        const auto& reg = m_regions[current];
        for (int i = 0; i < reg.portalCount; ++i) {
            uint16_t portalIdx = reg.portalStart + i;
            if (portalIdx >= m_header.portalCount) break;

            uint16_t neighbor = m_portals[portalIdx].neighborRegion;
            if (neighbor >= m_header.regionCount) continue;
            if (closedSet[neighbor]) continue;

            int32_t tentativeG = gScore[current] + edgeCost(current, neighbor);
            if (!openSet[neighbor] || tentativeG < gScore[neighbor]) {
                cameFrom[neighbor] = current;
                gScore[neighbor] = tentativeG;
                fScore[neighbor] = tentativeG + regionHeuristic(neighbor, endRegion);
                openSet[neighbor] = 1;
            }
        }
    }
}

// ============================================================================
// Get Y Distance (For region and player)
// ============================================================================

int32_t NavRegionSystem::getYDistance(int32_t firstY, int32_t secondY){
    int32_t result = firstY - secondY;
    return result < 0 ? -result : result;
}

// ============================================================================
// Region flag accessors
// ============================================================================

bool NavRegionSystem::isRegionPlatform(uint16_t regionIndex) const {
    if (regionIndex >= m_header.regionCount) return false;
    return (m_regions[regionIndex].flags & NAV_FLAG_PLATFORM) != 0;
}

uint8_t NavRegionSystem::getWalkoffEdgeMask(uint16_t regionIndex) const {
    if (regionIndex >= m_header.regionCount) return 0;
    return m_regions[regionIndex].walkoffEdgeMask;
}

}  // namespace psxsplash
