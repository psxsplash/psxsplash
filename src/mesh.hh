#pragma once

#include "psyqo/gte-registers.hh"
#include "psyqo/primitives/common.hh"

namespace psxsplash {

class Tri final {
  public:
    psyqo::GTE::PackedVec3 v0, v1, v2;
    psyqo::GTE::PackedVec3 normal;
    psyqo::PrimPieces::UVCoords uvA, uvB;
    psyqo::PrimPieces::UVCoordsPadded uvC;
    psyqo::Color colorA, colorB, colorC;
};

} // namespace psxsplash
