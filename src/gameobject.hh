#pragma once

#include "psyqo/matrix.hh"
#include "psyqo/vector.hh"

#include "mesh.hh"

namespace psxsplash {

class GameObject final {
  public:
    psyqo::Vec3 position;
    psyqo::Matrix33 rotation;
    psyqo::PrimPieces::TPageAttr texture;
    uint16_t polyCount;
    union {
        Tri *polygons;
        uint32_t polygonsOffset;
    };
};
} // namespace psxsplash