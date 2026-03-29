#pragma once

/**
 * worldcollision.hh - Player-vs-World Triangle Collision
 *
 * Architecture:
 *   1. Broad phase: per-mesh AABB reject, then spatial grid (exterior) or
 *      room membership (interior) to narrow candidate meshes.
 *   2. Narrow phase: per-triangle capsule-vs-triangle sweep.
 *   3. Response: sliding projection along collision plane.
 *
 * All math is fixed-point 20.12. Zero floats. Deterministic at any framerate.
 */

#include <stdint.h>
#include <psyqo/fixed-point.hh>
#include <psyqo/vector.hh>

namespace psxsplash {

// ============================================================================
// Surface flags — packed per-triangle, exported from SplashEdit
// ============================================================================
enum SurfaceFlag : uint8_t {
    SURFACE_SOLID    = 0x01,
    SURFACE_SLOPE    = 0x02,
    SURFACE_STAIRS   = 0x04,
    SURFACE_NO_WALK  = 0x10,
};

// ============================================================================
// Collision triangle — world-space, pre-transformed, contiguous in memory
// 40 bytes each — v0(12) + v1(12) + v2(12) + normal(12) omitted to save
// Actually: 40 bytes = v0(12) + edge1(12) + edge2(12) + flags(1) + pad(3)
// We store edges for Moller-Trumbore intersection
// ============================================================================
struct CollisionTri {
    // Vertex 0 (world-space 20.12 fixed-point)
    int32_t v0x, v0y, v0z;     // 12 bytes
    // Edge1 = v1 - v0
    int32_t e1x, e1y, e1z;     // 12 bytes
    // Edge2 = v2 - v0
    int32_t e2x, e2y, e2z;     // 12 bytes
    // Precomputed face normal (unit-ish, 20.12)
    int32_t nx, ny, nz;        // 12 bytes
    // Surface properties
    uint8_t  flags;             // SurfaceFlag bitmask
    uint8_t  roomIndex;         // Room/chunk this tri belongs to (0xFF = none)
    uint16_t pad;               // Alignment
};
static_assert(sizeof(CollisionTri) == 52, "CollisionTri must be 52 bytes");

// ============================================================================
// Collision mesh header — one per collision mesh in the splashpack
// The triangles themselves follow contiguously after all headers.
// ============================================================================
struct CollisionMeshHeader {
    // World-space AABB for broad-phase rejection (20.12 fixed-point)
    int32_t aabbMinX, aabbMinY, aabbMinZ;  // 12 bytes
    int32_t aabbMaxX, aabbMaxY, aabbMaxZ;  // 12 bytes
    // Offset into the collision triangle array
    uint16_t firstTriangle;     // Index of first CollisionTri
    uint16_t triangleCount;     // Number of triangles
    // Room/chunk association
    uint8_t  roomIndex;         // Interior room index (0xFF = exterior)
    uint8_t  pad[3];
};
static_assert(sizeof(CollisionMeshHeader) == 32, "CollisionMeshHeader must be 32 bytes");

// ============================================================================
// Spatial chunk for exterior scenes — 2D grid over XZ
// ============================================================================
struct CollisionChunk {
    uint16_t firstMeshIndex;    // Index into CollisionMeshHeader array
    uint16_t meshCount;         // Number of meshes in this chunk
};
static_assert(sizeof(CollisionChunk) == 4, "CollisionChunk must be 4 bytes");

// ============================================================================
// Collision data header — describes the entire collision dataset
// ============================================================================
struct CollisionDataHeader {
    uint16_t meshCount;         // Number of CollisionMeshHeader entries
    uint16_t triangleCount;     // Total CollisionTri entries
    uint16_t chunkGridW;        // Spatial grid width (0 if interior)
    uint16_t chunkGridH;        // Spatial grid height (0 if interior)
    int32_t  chunkOriginX;      // Grid origin X (20.12)
    int32_t  chunkOriginZ;      // Grid origin Z (20.12)
    int32_t  chunkSize;         // Cell size (20.12)
    // Total: 20 bytes
    // Followed by: meshCount * CollisionMeshHeader
    //              triangleCount * CollisionTri
    //              chunkGridW * chunkGridH * CollisionChunk (if exterior)
};
static_assert(sizeof(CollisionDataHeader) == 20, "CollisionDataHeader must be 20 bytes");

// ============================================================================
// Hit result from collision queries
// ============================================================================
struct CollisionHit {
    int32_t pointX, pointY, pointZ;     // Hit point (20.12)
    int32_t normalX, normalY, normalZ;  // Hit normal (20.12)
    int32_t distance;                   // Distance along ray (20.12)
    uint16_t triangleIndex;             // Which triangle was hit
    uint8_t  surfaceFlags;              // SurfaceFlag of hit triangle
    uint8_t  pad;
};

// ============================================================================
// Maximum slope angle for walkable surfaces
// cos(46°) ≈ 0.6947 → in 20.12 fixed-point = 2845
// Surfaces with normal.y < this are treated as walls
// ============================================================================
static constexpr int32_t WALKABLE_SLOPE_COS = 2845;  // cos(46°) in 20.12

// Player collision capsule radius (20.12 fixed-point)
// ~0.5 world units at GTEScaling=100 → 0.005 GTE units → 20 in 20.12
static constexpr int32_t PLAYER_RADIUS = 20;

// Small epsilon for collision (20.12)
// ≈ 0.01 GTE units
static constexpr int32_t COLLISION_EPSILON = 41;

// Maximum number of collision iterations per frame
static constexpr int MAX_COLLISION_ITERATIONS = 8;

// Maximum triangles to test per frame (budget)
static constexpr int MAX_TRI_TESTS_PER_FRAME = 256;

// ============================================================================
// WorldCollision — main collision query interface
// Loaded from splashpack data, used by SceneManager every frame
// ============================================================================
class WorldCollision {
public:
    WorldCollision() = default;

    /// Initialize from splashpack data. Returns pointer past the data.
    const uint8_t* initializeFromData(const uint8_t* data);

    /// Is collision data loaded?
    bool isLoaded() const { return m_triangles != nullptr; }

    void relocate(intptr_t delta) {
        if (m_meshes) m_meshes = reinterpret_cast<const CollisionMeshHeader*>(reinterpret_cast<intptr_t>(m_meshes) + delta);
        if (m_triangles) m_triangles = reinterpret_cast<const CollisionTri*>(reinterpret_cast<intptr_t>(m_triangles) + delta);
        if (m_chunks) m_chunks = reinterpret_cast<const CollisionChunk*>(reinterpret_cast<intptr_t>(m_chunks) + delta);
    }

    // ========================================================================
    // High-level queries used by the player movement system
    // ========================================================================

    /// Move a sphere from oldPos to newPos, sliding against world geometry.
    /// Returns the final valid position after collision response.
    /// radius is in 20.12 fixed-point.
    psyqo::Vec3 moveAndSlide(const psyqo::Vec3& oldPos,
                             const psyqo::Vec3& newPos,
                             int32_t radius,
                             uint8_t currentRoom) const;

    /// Cast a ray downward from pos to find the ground.
    /// Returns true if ground found within maxDist.
    /// groundY and groundNormal are filled on hit.
    bool groundTrace(const psyqo::Vec3& pos,
                     int32_t maxDist,
                     int32_t& groundY,
                     int32_t& groundNormalY,
                     uint8_t& surfaceFlags,
                     uint8_t currentRoom) const;

    /// Cast a ray upward to detect ceilings.
    bool ceilingTrace(const psyqo::Vec3& pos,
                      int32_t playerHeight,
                      int32_t& ceilingY,
                      uint8_t currentRoom) const;

    /// Raycast against collision geometry. Returns true on hit.
    bool raycast(int32_t ox, int32_t oy, int32_t oz,
                 int32_t dx, int32_t dy, int32_t dz,
                 int32_t maxDist,
                 CollisionHit& hit,
                 uint8_t currentRoom) const;

    /// Get mesh count for debugging
    uint16_t getMeshCount() const { return m_header.meshCount; }
    uint16_t getTriangleCount() const { return m_header.triangleCount; }

private:
    CollisionDataHeader m_header = {};
    const CollisionMeshHeader* m_meshes = nullptr;
    const CollisionTri* m_triangles = nullptr;
    const CollisionChunk* m_chunks = nullptr;  // Only for exterior scenes

    /// Collect candidate mesh indices near a position.
    /// For exterior: uses spatial grid. For interior: uses roomIndex.
    int gatherCandidateMeshes(int32_t posX, int32_t posZ,
                              uint8_t currentRoom,
                              uint16_t* outIndices,
                              int maxIndices) const;

    /// Test a sphere against a single triangle. Returns penetration depth (>0 if colliding).
    /// On collision, fills outNormal with the push-out direction.
    int32_t sphereVsTriangle(int32_t cx, int32_t cy, int32_t cz,
                             int32_t radius,
                             const CollisionTri& tri,
                             int32_t& outNx, int32_t& outNy, int32_t& outNz) const;

    /// Ray vs triangle (Moller-Trumbore in fixed-point).
    /// Returns distance along ray (20.12), or -1 if no hit.
    int32_t rayVsTriangle(int32_t ox, int32_t oy, int32_t oz,
                          int32_t dx, int32_t dy, int32_t dz,
                          const CollisionTri& tri) const;

    /// AABB vs AABB test
    static bool aabbOverlap(int32_t aMinX, int32_t aMinY, int32_t aMinZ,
                            int32_t aMaxX, int32_t aMaxY, int32_t aMaxZ,
                            int32_t bMinX, int32_t bMinY, int32_t bMinZ,
                            int32_t bMaxX, int32_t bMaxY, int32_t bMaxZ);

    /// Expand a point to an AABB with radius
    static void sphereToAABB(int32_t cx, int32_t cy, int32_t cz, int32_t r,
                             int32_t& minX, int32_t& minY, int32_t& minZ,
                             int32_t& maxX, int32_t& maxY, int32_t& maxZ);
};

}  // namespace psxsplash
