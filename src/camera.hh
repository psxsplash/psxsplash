#pragma once

#include <sys/types.h>

#include <psyqo/fixed-point.hh>
#include <psyqo/matrix.hh>
#include <psyqo/trigonometry.hh>

namespace psxsplash {

class Camera {
  public:
    Camera();

    void moveX(psyqo::FixedPoint<12> x);
    void moveY(psyqo::FixedPoint<12> y);
    void moveZ(psyqo::FixedPoint<12> y);

    void setPosition(psyqo::FixedPoint<12> x, psyqo::FixedPoint<12> y, psyqo::FixedPoint<12> z);

    psyqo::Vec3& getPosition() { return m_rotation; }

    void rotateX(psyqo::Angle x);
    void rotateY(psyqo::Angle y);
    void rotateZ(psyqo::Angle z);

    void setRotation(psyqo::Angle x, psyqo::Angle y, psyqo::Angle z);
    psyqo::Matrix33& getRotation();

  private:
    psyqo::Matrix33 m_rotationMatrix;
    psyqo::Trig<> m_trig;
    psyqo::Vec3 m_rotation;
};
}  // namespace psxsplash