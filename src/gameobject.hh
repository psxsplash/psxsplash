#pragma once

#include "psyqo/trigonometry.hh"
#include "psyqo/vector.hh"

#include "mesh.hh"

namespace psxsplash {

class GameObject final {
  public:
    psyqo::Vec3 m_pos;
    psyqo::Angle m_rot[3];
    Mesh m_mesh;
    bool m_is_static;
};
} // namespace psxsplash