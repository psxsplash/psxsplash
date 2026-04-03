#pragma once

#include <stdint.h>
#include <psyqo/fixed-point.hh>
#include <psyqo/vector.hh>

namespace psxsplash {

// Forward declaration for Frustum::testAABB overload
struct RoomCell;

/// Triangle reference - points to a specific triangle in a specific object
struct TriangleRef {
    uint16_t objectIndex;
    uint16_t triangleIndex;
};
static_assert(sizeof(TriangleRef) == 4, "TriangleRef must be 4 bytes");

/// BVH Node - stored in binary file
struct BVHNode {
    int32_t minX, minY, minZ;  
    int32_t maxX, maxY, maxZ; 

    uint16_t leftChild; 
    uint16_t rightChild; 

    uint16_t firstTriangle; 
    uint16_t triangleCount; 

    bool isLeaf() const {
        return leftChild == 0xFFFF && rightChild == 0xFFFF;
    }

    bool testPlane(int32_t nx, int32_t ny, int32_t nz, int32_t d) const {
        int32_t px = (nx >= 0) ? maxX : minX;
        int32_t py = (ny >= 0) ? maxY : minY;
        int32_t pz = (nz >= 0) ? maxZ : minZ;

        int64_t dot = ((int64_t)px * nx + (int64_t)py * ny + (int64_t)pz * nz) >> 12;
        return (dot + d) >= 0;
    }
};
static_assert(sizeof(BVHNode) == 32, "BVHNode must be 32 bytes");

/// BVH Tree header in binary file
struct BVHHeader {
    uint16_t nodeCount;
    uint16_t triangleRefCount;
};
static_assert(sizeof(BVHHeader) == 4, "BVHHeader must be 4 bytes");

/// Frustum planes for culling (6 planes)
struct Frustum {
    struct Plane {
        int32_t nx, ny, nz, d;
    };
    Plane planes[6]; 

    bool testAABB(const BVHNode& node) const {
        for (int i = 0; i < 6; i++) {
            if (!node.testPlane(planes[i].nx, planes[i].ny, planes[i].nz, planes[i].d)) {
                return false;  
            }
        }
        return true;
    }

    // RoomCell overload defined after RoomCell struct (see below)
    inline bool testAABB(const RoomCell& cell) const;
};

/// BVH Manager - handles traversal and culling
class BVHManager {
public:
    /// Initialize from separate pointers (used by splashpack loader)
    void initialize(const BVHNode* nodes, uint16_t nodeCount,
                   const TriangleRef* triangleRefs, uint16_t triangleRefCount);

    /// Traverse BVH and collect visible triangle references
    /// Uses frustum culling to skip invisible branches
    /// Returns number of visible triangle refs
    int cullFrustum(const Frustum& frustum,
                    TriangleRef* outRefs,
                    int maxRefs) const;

    int queryRegion(int32_t minX, int32_t minY, int32_t minZ,
                    int32_t maxX, int32_t maxY, int32_t maxZ,
                    TriangleRef* outRefs,
                    int maxRefs) const;

    /// Get node count
    int getNodeCount() const { return m_nodeCount; }

    /// Get triangle ref count
    int getTriangleRefCount() const { return m_triangleRefCount; }

    /// Check if BVH is loaded
    bool isLoaded() const { return m_nodes != nullptr; }

    void relocate(intptr_t delta) {
        if (m_nodes) m_nodes = reinterpret_cast<const BVHNode*>(reinterpret_cast<intptr_t>(m_nodes) + delta);
        if (m_triangleRefs) m_triangleRefs = reinterpret_cast<const TriangleRef*>(reinterpret_cast<intptr_t>(m_triangleRefs) + delta);
    }

private:
    const BVHNode* m_nodes = nullptr;
    const TriangleRef* m_triangleRefs = nullptr;
    uint16_t m_nodeCount = 0;
    uint16_t m_triangleRefCount = 0;

    /// Recursive frustum culling traversal
    int traverseFrustum(int nodeIndex,
                        const Frustum& frustum,
                        TriangleRef* outRefs,
                        int currentCount,
                        int maxRefs) const;

    /// Recursive region query traversal
    int traverseRegion(int nodeIndex,
                       int32_t qMinX, int32_t qMinY, int32_t qMinZ,
                       int32_t qMaxX, int32_t qMaxY, int32_t qMaxZ,
                       TriangleRef* outRefs,
                       int currentCount,
                       int maxRefs) const;

    /// Test if two AABBs overlap
    static bool aabbOverlap(const BVHNode& node,
                            int32_t qMinX, int32_t qMinY, int32_t qMinZ,
                            int32_t qMaxX, int32_t qMaxY, int32_t qMaxZ);
};

// ── Room/portal data for interior scene occlusion ──

/// Per-room data loaded from splashpack v11+.
/// AABB for point-in-room tests plus a range into the room triangle-ref array.
struct RoomData {
    int32_t aabbMinX, aabbMinY, aabbMinZ;  // 12 bytes
    int32_t aabbMaxX, aabbMaxY, aabbMaxZ;  // 12 bytes
    uint16_t firstTriRef;                   // 2 bytes - index into room tri-ref array
    uint16_t triRefCount;                   // 2 bytes
    uint16_t firstCell;                     // 2 bytes - index into cell array
    uint8_t  cellCount;                     // 1 byte  - typically 8 (2×2×2)
    uint8_t  portalRefCount;                // 1 byte  - number of portal refs for this room
    uint16_t firstPortalRef;                // 2 bytes - index into room-portal-ref array
    uint16_t pad;                           // 2 bytes - alignment
};
static_assert(sizeof(RoomData) == 36, "RoomData must be 36 bytes");

/// Per-room portal reference — maps a room to its adjacent portals.
/// Stored in a flat array indexed by RoomData::firstPortalRef + i.
struct RoomPortalRef {
    uint16_t portalIndex;   // index into the portal array
    uint16_t otherRoom;     // room on the other side
};
static_assert(sizeof(RoomPortalRef) == 4, "RoomPortalRef must be 4 bytes");

/// Per-room spatial cell for sub-room frustum culling.
/// Each cell covers a fraction of a room's volume (typically 2×2×2 = 8 cells per room).
/// At runtime, frustum-test each cell's AABB to skip triangles in invisible sub-volumes.
struct RoomCell {
    int32_t minX, minY, minZ;   // 12 bytes - tight AABB around cell's actual triangles
    int32_t maxX, maxY, maxZ;   // 12 bytes
    uint16_t firstTriRef;        // 2 bytes  - index into room tri-ref array
    uint16_t triRefCount;        // 2 bytes

    /// P-vertex frustum plane test (same algorithm as BVHNode::testPlane)
    bool testPlane(int32_t nx, int32_t ny, int32_t nz, int32_t d) const {
        int32_t px = (nx >= 0) ? maxX : minX;
        int32_t py = (ny >= 0) ? maxY : minY;
        int32_t pz = (nz >= 0) ? maxZ : minZ;
        int64_t dot = ((int64_t)px * nx + (int64_t)py * ny + (int64_t)pz * nz) >> 12;
        return (dot + d) >= 0;
    }
};
static_assert(sizeof(RoomCell) == 28, "RoomCell must be 28 bytes");

// Deferred implementation of Frustum::testAABB(RoomCell) — needs full RoomCell definition.
inline bool Frustum::testAABB(const RoomCell& cell) const {
    for (int i = 0; i < 6; i++) {
        if (!cell.testPlane(planes[i].nx, planes[i].ny, planes[i].nz, planes[i].d)) {
            return false;
        }
    }
    return true;
}

/// Per-portal data connecting two rooms.
/// Center position is in fixed-point world/GTE space (20.12).
/// halfW/halfH define the portal opening size.
/// Normal, right, and up define the portal's orientation in world space.
/// Corner vertices are computed as: center +/- right*halfW +/- up*halfH.
struct PortalData {
    uint16_t roomA;                          // 2 bytes
    uint16_t roomB;                          // 2 bytes
    int32_t centerX, centerY, centerZ;       // 12 bytes - portal center (20.12 fp)
    int16_t halfW;                           // 2 bytes - half-width in GTE units (4.12 fp)
    int16_t halfH;                           // 2 bytes - half-height in GTE units (4.12 fp)
    int16_t normalX, normalY, normalZ;       // 6 bytes - facing direction (4.12 fp unit vector)
    int16_t pad;                             // 2 bytes - alignment
    int16_t rightX, rightY, rightZ;          // 6 bytes - local right axis (4.12 fp unit vector)
    int16_t upX, upY, upZ;                   // 6 bytes - local up axis (4.12 fp unit vector)
};
static_assert(sizeof(PortalData) == 40, "PortalData must be 40 bytes");

}  // namespace psxsplash
