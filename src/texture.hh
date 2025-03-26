#pragma once

#include <cstdint>
#include <psyqo/primitives/common.hh>

namespace psxsplash {
class Texture final {
  public:
    psyqo::PrimPieces::TPageAttr m_tpage;
    uint8_t m_width, m_height;
};

}  // namespace psxsplash