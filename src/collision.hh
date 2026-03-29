#pragma once

#include <psyqo/fixed-point.hh>
#include <psyqo/vector.hh>
#include <EASTL/vector.h>

#include "gameobject.hh"

namespace psxsplash {

class SceneManager;

enum class CollisionType : uint8_t {
    None = 0,
    Solid = 1,
};

using CollisionMask = uint8_t;

struct AABB {
    psyqo::Vec3 min;
    psyqo::Vec3 max;
    
    bool intersects(const AABB& other) const {
        return (min.x <= other.max.x && max.x >= other.min.x) &&
               (min.y <= other.max.y && max.y >= other.min.y) &&
               (min.z <= other.max.z && max.z >= other.min.z);
    }
    
    bool contains(const psyqo::Vec3& point) const {
        return (point.x >= min.x && point.x <= max.x) &&
               (point.y >= min.y && point.y <= max.y) &&
               (point.z >= min.z && point.z <= max.z);
    }
    
    void expand(const psyqo::Vec3& delta);
};
static_assert(sizeof(AABB) == 24);

struct CollisionData {
    AABB localBounds;
    AABB bounds;
    CollisionType type;
    CollisionMask layerMask;
    uint16_t gameObjectIndex;
};

struct CollisionResult {
    uint16_t objectA;
    uint16_t objectB;
    psyqo::Vec3 normal;
    psyqo::FixedPoint<12> penetration;
};

struct TriggerBoxData {
    AABB bounds;
    int16_t luaFileIndex;
    uint16_t padding;
};

struct TriggerPair {
    uint16_t triggerIndex;
    uint16_t padding;
    uint8_t framesSinceContact;
    uint8_t state; // 0=new(enter), 1=active, 2=exiting
    uint16_t padding2;
};

class SpatialGrid {
public:
    static constexpr int GRID_SIZE = 8;
    static constexpr int CELL_COUNT = GRID_SIZE * GRID_SIZE * GRID_SIZE;
    static constexpr int MAX_OBJECTS_PER_CELL = 16;

    static psyqo::FixedPoint<12> WORLD_MIN;
    static psyqo::FixedPoint<12> WORLD_MAX;
    static psyqo::FixedPoint<12> CELL_SIZE;

    struct Cell {
        uint16_t objectIndices[MAX_OBJECTS_PER_CELL];
        uint8_t count;
        uint8_t padding[3];
    };

    void clear();
    void insert(uint16_t objectIndex, const AABB& bounds);
    int queryAABB(const AABB& bounds, uint16_t* output, int maxResults) const;

private:
    Cell m_cells[CELL_COUNT];
    void worldToGrid(const psyqo::Vec3& pos, int& gx, int& gy, int& gz) const;
};

class CollisionSystem {
public:
    static constexpr int MAX_COLLIDERS = 64;
    static constexpr int MAX_TRIGGER_BOXES = 32;
    static constexpr int MAX_TRIGGER_PAIRS = 32;
    static constexpr int MAX_COLLISION_RESULTS = 32;

    CollisionSystem() = default;

    void init();
    void reset();

    void registerCollider(uint16_t gameObjectIndex, const AABB& localBounds,
                          CollisionType type, CollisionMask mask);
    void updateCollider(uint16_t gameObjectIndex, const psyqo::Vec3& position,
                        const psyqo::Matrix33& rotation);

    void registerTriggerBox(const AABB& bounds, int16_t luaFileIndex);

    int detectCollisions(const AABB& playerAABB, psyqo::Vec3& pushBack, class SceneManager& scene);

    void detectTriggers(const AABB& playerAABB, class SceneManager& scene);

    const CollisionResult* getResults() const { return m_results; }
    int getResultCount() const { return m_resultCount; }

    int getColliderCount() const { return m_colliderCount; }

private:
    CollisionData m_colliders[MAX_COLLIDERS];
    int m_colliderCount = 0;

    TriggerBoxData m_triggerBoxes[MAX_TRIGGER_BOXES];
    int m_triggerBoxCount = 0;

    SpatialGrid m_grid;

    CollisionResult m_results[MAX_COLLISION_RESULTS];
    int m_resultCount = 0;

    TriggerPair m_triggerPairs[MAX_TRIGGER_PAIRS];
    int m_triggerPairCount = 0;

    bool testAABB(const AABB& a, const AABB& b,
                  psyqo::Vec3& normal, psyqo::FixedPoint<12>& penetration) const;
};

} // namespace psxsplash