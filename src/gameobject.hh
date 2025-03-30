#pragma once

#include <cstdint>
#include <psyqo/matrix.hh>
#include <psyqo/vector.hh>

#include "mesh.hh"

namespace psxsplash {

class GameObject final {
  public:
    psyqo::Vec3 position;
    psyqo::Matrix33 rotation;
    union {
        Tri *polygons;
        uint32_t polygonsOffset;
    };
    int polyCount;
};
static_assert(sizeof(GameObject) == 56, "GameObject is not 56 bytes");
}  // namespace psxsplash