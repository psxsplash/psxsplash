#pragma once

#include <psyqo/matrix.hh>
#include <psyqo/vector.hh>

#include "mesh.hh"

namespace psxsplash {

class GameObject final {
  public:
    union {
        Tri *polygons;
        uint32_t polygonsOffset;
    };
    psyqo::Vec3 position;
    psyqo::Matrix33 rotation;
    uint16_t polyCount;
    int16_t luaFileIndex;
};
static_assert(sizeof(GameObject) == 56, "GameObject is not 56 bytes");
}  // namespace psxsplash