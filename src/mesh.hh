#pragma once

#include "EASTL/array.h"
#include "psyqo/gte-registers.hh"
#include "psyqo/primitives/common.hh"
#include "texture.hh"

namespace psxsplash {

class Tri final {
    public:
    psyqo::GTE::PackedVec3 v0,v1,v2;
    psyqo::GTE::PackedVec3 n0,n1,n2;
    psyqo::PrimPieces::UVCoords uvA, uvB;
    psyqo::PrimPieces::UVCoordsPadded uvC;
    psyqo::Color colorA, colorB, colorC;
};

class Mesh final {
    public:
    Texture m_texture;
    eastl::array<Tri> m_polygons;
};
}