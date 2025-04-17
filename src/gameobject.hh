#pragma once

#include <common/util/bitfield.hh>
#include <psyqo/matrix.hh>
#include <psyqo/vector.hh>

#include "mesh.hh"

namespace psxsplash {

// LSB is active in flags

class GameObject final {
    typedef Utilities::BitSpan<bool> IsActive;
    typedef Utilities::BitField<IsActive> GameObjectFlags;
  public:
    union {
        Tri *polygons;
        uint32_t polygonsOffset;
    };
    psyqo::Vec3 position;
    psyqo::Matrix33 rotation;
    // linear & angular velocity placeholders
    uint16_t polyCount;
    int16_t luaFileIndex;
    union {
        GameObjectFlags flags;
        uint32_t flagsAsInt;
    };
    bool isActive() const { return flags.get<IsActive>(); }
    void setActive(bool active) { flags.set<IsActive>(active); }
};
static_assert(sizeof(GameObject) == 60, "GameObject is not 56 bytes");
}  // namespace psxsplash