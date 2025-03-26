#pragma once

#include <cstdint>
#include <psyqo/matrix.hh>
#include <psyqo/vector.hh>

#include "mesh.hh"

namespace psxsplash {

class GameObject final {
  public:
    psyqo::Vec3 position;                  // 12 bytes
    psyqo::Matrix33 rotation;              // 36 bytes
    uint16_t polyCount;                    // 2 bytes
    psyqo::PrimPieces::TPageAttr texture;  // 2 bytes
    uint16_t clutX, clutY;
    uint16_t clut[256];
    union {  // 4 bytes
        Tri *polygons;
        uint32_t polygonsOffset;
    };
};
}  // namespace psxsplash