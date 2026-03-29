#include "collision.hh"
#include "scenemanager.hh"

#include <psyqo/fixed-point.hh>

using FP = psyqo::FixedPoint<12>;

namespace psxsplash {

psyqo::FixedPoint<12> SpatialGrid::WORLD_MIN = FP(-16);
psyqo::FixedPoint<12> SpatialGrid::WORLD_MAX = FP(16);
psyqo::FixedPoint<12> SpatialGrid::CELL_SIZE = FP(4);

void AABB::expand(const psyqo::Vec3& delta) {
    psyqo::FixedPoint<12> zero;
    if (delta.x > zero) max.x = max.x + delta.x;
    else min.x = min.x + delta.x;
    if (delta.y > zero) max.y = max.y + delta.y;
    else min.y = min.y + delta.y;
    if (delta.z > zero) max.z = max.z + delta.z;
    else min.z = min.z + delta.z;
}

// ============================================================================
// SpatialGrid Implementation
// ============================================================================

void SpatialGrid::clear() {
    for (int i = 0; i < CELL_COUNT; i++) {
        m_cells[i].count = 0;
    }
}

void SpatialGrid::worldToGrid(const psyqo::Vec3& pos, int& gx, int& gy, int& gz) const {
    auto px = pos.x;
    auto py = pos.y;
    auto pz = pos.z;
    
    if (px < WORLD_MIN) px = WORLD_MIN;
    if (px > WORLD_MAX) px = WORLD_MAX;
    if (py < WORLD_MIN) py = WORLD_MIN;
    if (py > WORLD_MAX) py = WORLD_MAX;
    if (pz < WORLD_MIN) pz = WORLD_MIN;
    if (pz > WORLD_MAX) pz = WORLD_MAX;
    
    gx = ((px - WORLD_MIN) / CELL_SIZE).integer();
    gy = ((py - WORLD_MIN) / CELL_SIZE).integer();
    gz = ((pz - WORLD_MIN) / CELL_SIZE).integer();
    
    if (gx < 0) gx = 0;
    if (gx >= GRID_SIZE) gx = GRID_SIZE - 1;
    if (gy < 0) gy = 0;
    if (gy >= GRID_SIZE) gy = GRID_SIZE - 1;
    if (gz < 0) gz = 0;
    if (gz >= GRID_SIZE) gz = GRID_SIZE - 1;
}

void SpatialGrid::insert(uint16_t objectIndex, const AABB& bounds) {
    int minGx, minGy, minGz;
    int maxGx, maxGy, maxGz;
    
    worldToGrid(bounds.min, minGx, minGy, minGz);
    worldToGrid(bounds.max, maxGx, maxGy, maxGz);
    
    for (int gz = minGz; gz <= maxGz; gz++) {
        for (int gy = minGy; gy <= maxGy; gy++) {
            for (int gx = minGx; gx <= maxGx; gx++) {
                int cellIndex = gx + gy * GRID_SIZE + gz * GRID_SIZE * GRID_SIZE;
                Cell& cell = m_cells[cellIndex];
                
                if (cell.count < MAX_OBJECTS_PER_CELL) {
                    cell.objectIndices[cell.count++] = objectIndex;
                }
            }
        }
    }
}

int SpatialGrid::queryAABB(const AABB& bounds, uint16_t* output, int maxResults) const {
    int resultCount = 0;
    
    int minGx, minGy, minGz;
    int maxGx, maxGy, maxGz;
    
    worldToGrid(bounds.min, minGx, minGy, minGz);
    worldToGrid(bounds.max, maxGx, maxGy, maxGz);
    
    uint32_t addedMaskLow = 0;
    uint32_t addedMaskHigh = 0;
    
    for (int gz = minGz; gz <= maxGz; gz++) {
        for (int gy = minGy; gy <= maxGy; gy++) {
            for (int gx = minGx; gx <= maxGx; gx++) {
                int cellIndex = gx + gy * GRID_SIZE + gz * GRID_SIZE * GRID_SIZE;
                const Cell& cell = m_cells[cellIndex];
                
                for (int i = 0; i < cell.count; i++) {
                    uint16_t objIndex = cell.objectIndices[i];
                    
                    if (objIndex < 32) {
                        uint32_t bit = 1U << objIndex;
                        if (addedMaskLow & bit) continue;
                        addedMaskLow |= bit;
                    } else if (objIndex < 64) {
                        uint32_t bit = 1U << (objIndex - 32);
                        if (addedMaskHigh & bit) continue;
                        addedMaskHigh |= bit;
                    }
                    
                    if (resultCount < maxResults) {
                        output[resultCount++] = objIndex;
                    }
                }
            }
        }
    }
    
    return resultCount;
}

// ============================================================================
// CollisionSystem Implementation
// ============================================================================

void CollisionSystem::init() {
    reset();
}

void CollisionSystem::reset() {
    m_colliderCount = 0;
    m_triggerBoxCount = 0;
    m_resultCount = 0;
    m_triggerPairCount = 0;
    m_grid.clear();
}

void CollisionSystem::registerCollider(uint16_t gameObjectIndex, const AABB& localBounds,
                                        CollisionType type, CollisionMask mask) {
    if (m_colliderCount >= MAX_COLLIDERS) return;

    CollisionData& data = m_colliders[m_colliderCount++];
    data.localBounds = localBounds;
    data.bounds = localBounds;
    data.type = type;
    data.layerMask = mask;
    data.gameObjectIndex = gameObjectIndex;
}

void CollisionSystem::registerTriggerBox(const AABB& bounds, int16_t luaFileIndex) {
    if (m_triggerBoxCount >= MAX_TRIGGER_BOXES) return;

    TriggerBoxData& tb = m_triggerBoxes[m_triggerBoxCount++];
    tb.bounds = bounds;
    tb.luaFileIndex = luaFileIndex;
}

void CollisionSystem::updateCollider(uint16_t gameObjectIndex, const psyqo::Vec3& position,
                                      const psyqo::Matrix33& rotation) {
    for (int i = 0; i < m_colliderCount; i++) {
        if (m_colliders[i].gameObjectIndex == gameObjectIndex) {
            m_colliders[i].bounds.min = m_colliders[i].localBounds.min + position;
            m_colliders[i].bounds.max = m_colliders[i].localBounds.max + position;
            break;
        }
    }
}

int CollisionSystem::detectCollisions(const AABB& playerAABB, psyqo::Vec3& pushBack, SceneManager& scene) {
    m_resultCount = 0;
    const FP zero(0);
    pushBack = psyqo::Vec3{zero, zero, zero};

    // Rebuild spatial grid with active colliders only
    m_grid.clear();
    for (int i = 0; i < m_colliderCount; i++) {
        auto* go = scene.getGameObject(m_colliders[i].gameObjectIndex);
        if (go && go->isActive()) {
            m_grid.insert(i, m_colliders[i].bounds);
        }
    }
    
    // Test player AABB against all colliders for push-back
    uint16_t nearby[32];
    int nearbyCount = m_grid.queryAABB(playerAABB, nearby, 32);
    
    for (int j = 0; j < nearbyCount; j++) {
        int idx = nearby[j];
        const CollisionData& collider = m_colliders[idx];
        if (collider.type == CollisionType::None) continue;
        
        psyqo::Vec3 normal;
        psyqo::FixedPoint<12> penetration;
        
        if (testAABB(playerAABB, collider.bounds, normal, penetration)) {
            // Accumulate push-back along the separation normal
            pushBack.x = pushBack.x + normal.x * penetration;
            pushBack.y = pushBack.y + normal.y * penetration;
            pushBack.z = pushBack.z + normal.z * penetration;
            
            if (m_resultCount < MAX_COLLISION_RESULTS) {
                CollisionResult& result = m_results[m_resultCount++];
                result.objectA = 0xFFFF; // player
                result.objectB = collider.gameObjectIndex;
                result.normal = normal;
                result.penetration = penetration;
            }
        }
    }
    
    return m_resultCount;
}

void CollisionSystem::detectTriggers(const AABB& playerAABB, SceneManager& scene) {
    int writeIndex = 0;
    
    // Mark all existing pairs as potentially stale
    for (int i = 0; i < m_triggerPairCount; i++) {
        m_triggerPairs[i].framesSinceContact++;
    }
    
    // Test player against each trigger box
    for (int ti = 0; ti < m_triggerBoxCount; ti++) {
        const TriggerBoxData& tb = m_triggerBoxes[ti];
        
        if (!playerAABB.intersects(tb.bounds)) continue;
        
        // Find existing pair
        bool found = false;
        for (int pi = 0; pi < m_triggerPairCount; pi++) {
            if (m_triggerPairs[pi].triggerIndex == ti) {
                m_triggerPairs[pi].framesSinceContact = 0;
                if (m_triggerPairs[pi].state == 0) {
                    m_triggerPairs[pi].state = 1; // enter -> active
                }
                found = true;
                break;
            }
        }
        
        // New pair: enter
        if (!found && m_triggerPairCount < MAX_TRIGGER_PAIRS) {
            TriggerPair& pair = m_triggerPairs[m_triggerPairCount++];
            pair.triggerIndex = ti;
            pair.padding = 0;
            pair.framesSinceContact = 0;
            pair.state = 0;
            pair.padding2 = 0;
        }
    }
    
    // Process pairs: fire events and clean up exited pairs
    writeIndex = 0;
    for (int i = 0; i < m_triggerPairCount; i++) {
        TriggerPair& pair = m_triggerPairs[i];
        int16_t luaIdx = m_triggerBoxes[pair.triggerIndex].luaFileIndex;
        
        if (pair.state == 0) {
            // Enter
            scene.fireTriggerEnter(luaIdx, pair.triggerIndex);
            pair.state = 1;
            m_triggerPairs[writeIndex++] = pair;
        } else if (pair.framesSinceContact > 2) {
            // Exit
            scene.fireTriggerExit(luaIdx, pair.triggerIndex);
        } else {
            // Still inside, keep alive
            m_triggerPairs[writeIndex++] = pair;
        }
    }
    m_triggerPairCount = writeIndex;
}

bool CollisionSystem::testAABB(const AABB& a, const AABB& b,
                                psyqo::Vec3& normal, psyqo::FixedPoint<12>& penetration) const {
    if (a.max.x < b.min.x || a.min.x > b.max.x) return false;
    if (a.max.y < b.min.y || a.min.y > b.max.y) return false;
    if (a.max.z < b.min.z || a.min.z > b.max.z) return false;
    
    auto overlapX1 = a.max.x - b.min.x;
    auto overlapX2 = b.max.x - a.min.x;
    auto overlapY1 = a.max.y - b.min.y;
    auto overlapY2 = b.max.y - a.min.y;
    auto overlapZ1 = a.max.z - b.min.z;
    auto overlapZ2 = b.max.z - a.min.z;
    
    auto minOverlapX = (overlapX1 < overlapX2) ? overlapX1 : overlapX2;
    auto minOverlapY = (overlapY1 < overlapY2) ? overlapY1 : overlapY2;
    auto minOverlapZ = (overlapZ1 < overlapZ2) ? overlapZ1 : overlapZ2;
    
    const FP zero(0);
    const FP one(1);
    const FP negOne(-1);
    
    if (minOverlapX <= minOverlapY && minOverlapX <= minOverlapZ) {
        penetration = minOverlapX;
        normal = psyqo::Vec3{(overlapX1 < overlapX2) ? negOne : one, zero, zero};
    } else if (minOverlapY <= minOverlapZ) {
        penetration = minOverlapY;
        normal = psyqo::Vec3{zero, (overlapY1 < overlapY2) ? negOne : one, zero};
    } else {
        penetration = minOverlapZ;
        normal = psyqo::Vec3{zero, zero, (overlapZ1 < overlapZ2) ? negOne : one};
    }
    
    return true;
}

} // namespace psxsplash
