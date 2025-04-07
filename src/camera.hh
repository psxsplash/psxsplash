#pragma once

#include <psyqo/fixed-point.hh>
#include <psyqo/matrix.hh>
#include <psyqo/trigonometry.hh>

namespace psxsplash {

class Camera {
  public:
    Camera();

    void MoveX(psyqo::FixedPoint<12> x);
    void MoveY(psyqo::FixedPoint<12> y);
    void MoveZ(psyqo::FixedPoint<12> y);

    void SetPosition(psyqo::FixedPoint<12> x, psyqo::FixedPoint<12> y, psyqo::FixedPoint<12> z);
    psyqo::Vec3& GetPosition() { return m_position; }

    void SetRotation(psyqo::Angle x, psyqo::Angle y, psyqo::Angle z);
    psyqo::Matrix33& GetRotation();

  private:
    psyqo::Matrix33 m_rotationMatrix;
    psyqo::Trig<> m_trig;
    psyqo::Vec3 m_position;
};
}  // namespace psxsplash