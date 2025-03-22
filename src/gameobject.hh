#pragma once

#include "psyqo/matrix.hh"
#include "psyqo/vector.hh"

#include "mesh.hh"

namespace psxsplash {

class GameObject final {
  public:
    psyqo::Vec3 position; // 12 bytes
    psyqo::Matrix33 rotation; // 36 bytes
    psyqo::PrimPieces::TPageAttr texture; // 2 bytes
    uint16_t polyCount; // 2 bytes
    union { // 4 bytes
        Tri *polygons;
        uint32_t polygonsOffset;
    };
};
} // namespace psxsplash