#pragma once

#include <psyqo/matrix.hh>
#include <psyqo/vector.hh>

#include "mesh.hh"

namespace psxsplash {

class Lua;  // Forward declaration

// Component index constants - 0xFFFF means no component
constexpr uint16_t NO_COMPONENT = 0xFFFF;

/**
 * GameObject flags, one bit each. The exporter and SceneManager also read and
 * write these as raw masks on flagsAsInt, so the values here are the contract.
 *
 * Bit 0: active - whether object is active in scene
 * Bit 1: pendingEnable - flag for deferred enable (to batch Lua calls)
 * Bit 2: pendingDisable - flag for deferred disable
 * Bit 3: dynamicMoved - object position was changed at runtime (BVH stale)
 * Bit 4: skinned - drawn by the skinned-mesh pass
 * Bit 16: dynamicLit - point lights are applied to this mesh at runtime
 * Bit 17: dynamicLitSmooth - ... per vertex rather than per triangle
 */
class GameObject final {
    static constexpr uint32_t kActive = 0x01;
    static constexpr uint32_t kPendingEnable = 0x02;
    static constexpr uint32_t kPendingDisable = 0x04;
    static constexpr uint32_t kDynamicMoved = 0x08;
    static constexpr uint32_t kSkinned = 0x10;

    bool hasFlag(uint32_t f) const { return (flagsAsInt & f) != 0; }
    void setFlag(uint32_t f, bool on) { flagsAsInt = on ? (flagsAsInt | f) : (flagsAsInt & ~f); }

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
    
    uint32_t flagsAsInt;
    
    // Component indices (0xFFFF = no component)
    uint16_t interactableIndex;
    psyqo::PrimPieces::UVCoords uvOffset;       // Was healthIndex (legacy)
    // Runtime-only: Lua event bitmask (set during RegisterGameObject)
    // In the splashpack binary these 4 bytes are _reserved1 + _reserved2 (zeros).
    uint32_t eventMask;
    
    // World-space AABB (20.12 fixed-point, 24 bytes)
    // Used for per-object frustum culling before iterating triangles
    int32_t aabbMinX, aabbMinY, aabbMinZ;
    int32_t aabbMaxX, aabbMaxY, aabbMaxZ;
    
    // Basic accessors
    bool isActive() const { return hasFlag(kActive); }
    
    // setActive with Lua event support - call the version that takes Lua& for events
    void setActive(bool active) { setFlag(kActive, active); }
    
    // Deferred enable/disable for batched Lua calls
    bool isPendingEnable() const { return hasFlag(kPendingEnable); }
    bool isPendingDisable() const { return hasFlag(kPendingDisable); }
    void setPendingEnable(bool pending) { setFlag(kPendingEnable, pending); }
    void setPendingDisable(bool pending) { setFlag(kPendingDisable, pending); }
    
    // Dynamic movement tracking (BVH position stale)
    bool isDynamicMoved() const { return hasFlag(kDynamicMoved); }
    void setDynamicMoved(bool moved) { setFlag(kDynamicMoved, moved); }
    
    // Skinned mesh flag (bit 4)
    bool isSkinned() const { return hasFlag(kSkinned); }

    // Point lights affect this mesh. Skinned meshes ignore it. Bits 16 and 17
    // are raw masks written by the exporter, clear of the low flag bits.
    static constexpr uint32_t DYNAMIC_LIT_BIT = 0x10000;
    bool isDynamicLit() const { return (flagsAsInt & DYNAMIC_LIT_BIT) != 0; }
    // Bit 17: light per vertex instead of per triangle. About 9x the cost per
    // lit triangle in pcsx-redux; only meaningful with bit 16.
    static constexpr uint32_t DYNAMIC_LIT_SMOOTH_BIT = 0x20000;
    bool isDynamicLitSmooth() const { return (flagsAsInt & DYNAMIC_LIT_SMOOTH_BIT) != 0; }
    
    // Component checks
    bool hasInteractable() const { return interactableIndex != NO_COMPONENT; }
};
static_assert(sizeof(GameObject) == 92, "GameObject is not 92 bytes");

}  // namespace psxsplash