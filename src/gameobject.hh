#pragma once

#include <common/util/bitfield.hh>
#include <psyqo/matrix.hh>
#include <psyqo/vector.hh>

#include "mesh.hh"

namespace psxsplash {

class Lua;  // Forward declaration

// Component index constants - 0xFFFF means no component
constexpr uint16_t NO_COMPONENT = 0xFFFF;

/**
 * GameObject bitfield flags
 * 
 * Bit 0: isActive - whether object is active in scene
 * Bit 1: pendingEnable - flag for deferred enable (to batch Lua calls)
 * Bit 2: pendingDisable - flag for deferred disable
 * Bit 3: dynamicMoved - object position was changed at runtime (BVH stale)
 */
class GameObject final {
    typedef Utilities::BitSpan<bool> IsActive;
    typedef Utilities::BitSpan<bool, 1> PendingEnable;
    typedef Utilities::BitSpan<bool, 2> PendingDisable;
    typedef Utilities::BitSpan<bool, 3> DynamicMoved;
    typedef Utilities::BitField<IsActive, PendingEnable, PendingDisable, DynamicMoved> GameObjectFlags;
    
  public:
    union {
        Tri *polygons;
        uint32_t polygonsOffset;
    };
    psyqo::Vec3 position;
    psyqo::Matrix33 rotation;
    
    // Mesh data
    uint16_t polyCount;
    int16_t luaFileIndex;
    
    union {
        GameObjectFlags flags;
        uint32_t flagsAsInt;
    };
    
    // Component indices (0xFFFF = no component)
    uint16_t interactableIndex;
    uint16_t _reserved0;       // Was healthIndex (legacy, kept for binary layout)
    // Runtime-only: Lua event bitmask (set during RegisterGameObject)
    // In the splashpack binary these 4 bytes are _reserved1 + _reserved2 (zeros).
    uint32_t eventMask;
    
    // World-space AABB (20.12 fixed-point, 24 bytes)
    // Used for per-object frustum culling before iterating triangles
    int32_t aabbMinX, aabbMinY, aabbMinZ;
    int32_t aabbMaxX, aabbMaxY, aabbMaxZ;
    
    // Basic accessors
    bool isActive() const { return flags.get<IsActive>(); }
    
    // setActive with Lua event support - call the version that takes Lua& for events
    void setActive(bool active) { flags.set<IsActive>(active); }
    
    // Deferred enable/disable for batched Lua calls
    bool isPendingEnable() const { return flags.get<PendingEnable>(); }
    bool isPendingDisable() const { return flags.get<PendingDisable>(); }
    void setPendingEnable(bool pending) { flags.set<PendingEnable>(pending); }
    void setPendingDisable(bool pending) { flags.set<PendingDisable>(pending); }
    
    // Dynamic movement tracking (BVH position stale)
    bool isDynamicMoved() const { return flags.get<DynamicMoved>(); }
    void setDynamicMoved(bool moved) { flags.set<DynamicMoved>(moved); }
    
    // Component checks
    bool hasInteractable() const { return interactableIndex != NO_COMPONENT; }
};
static_assert(sizeof(GameObject) == 92, "GameObject is not 92 bytes");

}  // namespace psxsplash