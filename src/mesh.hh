#pragma once

#include <psyqo/gte-registers.hh>
#include <psyqo/primitives/common.hh>

namespace psxsplash {

  class Tri final {
    public:
      psyqo::GTE::PackedVec3 v0, v1, v2;  
      psyqo::GTE::PackedVec3 normal;      
  
      psyqo::Color colorA, colorB, colorC; 
  
      psyqo::PrimPieces::UVCoords uvA, uvB; 
      psyqo::PrimPieces::UVCoordsPadded uvC; 
  
      psyqo::PrimPieces::TPageAttr tpage; 
      uint16_t clutX;
      uint16_t clutY;
      uint16_t padding; 
  };
  static_assert(sizeof(Tri) == 52, "Tri is not 52 bytes");
  
} // namespace psxsplash
