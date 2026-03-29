#pragma once

/**
 * navregion.hh - Convex Region Navigation System
 *
 * Architecture:
 *   - Walkable surface decomposed into convex polygonal regions (XZ plane).
 *   - Adjacent regions share portal edges.
 *   - Player has a single current region index.
 *   - Movement: point-in-convex-polygon test → portal crossing → neighbor update.
 *   - Floor Y: project XZ onto region's floor plane.
 */

#include <stdint.h>
#include <psyqo/fixed-point.hh>
#include <psyqo/vector.hh>

namespace psxsplash {

// ============================================================================
// Constants
// ============================================================================
static constexpr int NAV_MAX_VERTS_PER_REGION = 8;   // Max polygon verts
static constexpr int NAV_MAX_NEIGHBORS = 8;           // Max portal edges per region
static constexpr int NAV_MAX_PATH_STEPS = 32;         // Max A* path length
static constexpr uint16_t NAV_NO_REGION = 0xFFFF;     // Sentinel: no region

// ============================================================================
// Surface type for navigation regions
// ============================================================================
enum NavSurfaceType : uint8_t {
    NAV_SURFACE_FLAT    = 0,
    NAV_SURFACE_RAMP    = 1,
    NAV_SURFACE_STAIRS  = 2,
};

// ============================================================================
// Portal edge — shared edge between two adjacent regions
// ============================================================================
struct NavPortal {
    int32_t  ax, az;            // Portal edge start (20.12 XZ)
    int32_t  bx, bz;            // Portal edge end (20.12 XZ)
    uint16_t neighborRegion;    // Index of the region on the other side
    int16_t  heightDelta;       // Vertical step in 4.12 (stairs, ledges)
};
static_assert(sizeof(NavPortal) == 20, "NavPortal must be 20 bytes");

// ============================================================================
// Nav Region — convex polygon on the XZ plane with floor info
// ============================================================================
struct NavRegion {
    // Convex polygon vertices (XZ, 20.12 fixed-point)
    // Stored in CCW winding order
    int32_t vertsX[NAV_MAX_VERTS_PER_REGION];  // 32 bytes
    int32_t vertsZ[NAV_MAX_VERTS_PER_REGION];  // 32 bytes

    // Floor plane: Y = planeA * X + planeB * Z + planeD (all 20.12)
    // For flat floors: planeA = planeB = 0, planeD = floorY
    int32_t planeA, planeB, planeD;             // 12 bytes

    // Portal neighbors
    uint16_t portalStart;   // Index into portal array    2 bytes
    uint8_t  portalCount;   // Number of portals          1 byte
    uint8_t  vertCount;     // Number of polygon verts    1 byte

    // Metadata
    NavSurfaceType surfaceType;  // 1 byte
    uint8_t  roomIndex;          // Interior room (0xFF = exterior)  1 byte
    uint8_t  pad[2];             // Alignment  2 bytes
    // Total: 32 + 32 + 12 + 4 + 4 = 84 bytes
};
static_assert(sizeof(NavRegion) == 84, "NavRegion must be 84 bytes");

// ============================================================================
// Nav data header
// ============================================================================
struct NavDataHeader {
    uint16_t regionCount;
    uint16_t portalCount;
    uint16_t startRegion;   // Region the player spawns in
    uint16_t pad;
};
static_assert(sizeof(NavDataHeader) == 8, "NavDataHeader must be 8 bytes");

// ============================================================================
// Path result for A* (used by NPC pathfinding)
// ============================================================================
struct NavPath {
    uint16_t regions[NAV_MAX_PATH_STEPS];
    int stepCount;
};

// ============================================================================
// NavRegionSystem — manages navigation at runtime
// ============================================================================
class NavRegionSystem {
public:
    NavRegionSystem() = default;

    /// Initialize from splashpack data. Returns pointer past the data.
    const uint8_t* initializeFromData(const uint8_t* data);

    /// Is nav data loaded?
    bool isLoaded() const { return m_regions != nullptr; }

    void relocate(intptr_t delta) {
        if (m_regions) m_regions = reinterpret_cast<const NavRegion*>(reinterpret_cast<intptr_t>(m_regions) + delta);
        if (m_portals) m_portals = reinterpret_cast<const NavPortal*>(reinterpret_cast<intptr_t>(m_portals) + delta);
    }

    /// Get the number of regions
    uint16_t getRegionCount() const { return m_header.regionCount; }

    /// Get the start region
    uint16_t getStartRegion() const { return m_header.startRegion; }

    /// Get the room index for a given nav region (0xFF = exterior/unknown)
    uint8_t getRoomIndex(uint16_t regionIndex) const {
        if (m_regions == nullptr || regionIndex >= m_header.regionCount) return 0xFF;
        return m_regions[regionIndex].roomIndex;
    }

    // ========================================================================
    // Player movement - called every frame
    // ========================================================================

    /// Given a new XZ position and the player's current region,
    /// determine the correct region and return the floor Y.
    /// Updates currentRegion in-place.
    /// newX/newZ are clamped to stay within the region boundary.
    /// Returns the Y position for the player's feet.
    int32_t resolvePosition(int32_t& newX, int32_t& newZ,
                            uint16_t& currentRegion) const;

    /// Test if a point (XZ) is inside a specific region.
    bool pointInRegion(int32_t x, int32_t z, uint16_t regionIndex) const;

    /// Compute floor Y at a given XZ within a region using the floor plane.
    int32_t getFloorY(int32_t x, int32_t z, uint16_t regionIndex) const;

    /// Find which region contains a point (brute-force, for initialization).
    uint16_t findRegion(int32_t x, int32_t z) const;

    /// Clamp an XZ position to stay within a region's polygon boundary.
    /// Returns the clamped position.
    void clampToRegion(int32_t& x, int32_t& z, uint16_t regionIndex) const;

    // TODO: Implement this
    bool findPath(uint16_t startRegion, uint16_t endRegion,
                  NavPath& path) const;

private:
    NavDataHeader m_header = {};
    const NavRegion* m_regions = nullptr;
    const NavPortal* m_portals = nullptr;

    /// Point-in-convex-polygon test (XZ plane).
    /// Uses cross-product sign consistency (all edges same winding).
    static bool pointInConvexPoly(int32_t px, int32_t pz,
                                  const int32_t* vertsX, const int32_t* vertsZ,
                                  int vertCount);

    /// Closest point on a line segment to a point (XZ only)
    static void closestPointOnSegment(int32_t px, int32_t pz,
                                      int32_t ax, int32_t az,
                                      int32_t bx, int32_t bz,
                                      int32_t& outX, int32_t& outZ);

};

}  // namespace psxsplash
