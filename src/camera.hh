#pragma once

#include <psyqo/fixed-point.hh>
#include <psyqo/matrix.hh>
#include <psyqo/trigonometry.hh>

#include "bvh.hh"

namespace psxsplash {

class Camera {
  public:
    Camera();

    void MoveX(psyqo::FixedPoint<12> x);
    void MoveY(psyqo::FixedPoint<12> y);
    void MoveZ(psyqo::FixedPoint<12> z);

    void SetPosition(psyqo::FixedPoint<12> x, psyqo::FixedPoint<12> y, psyqo::FixedPoint<12> z);
    psyqo::Vec3& GetPosition() { return m_position; }

    void SetRotation(psyqo::Angle x, psyqo::Angle y, psyqo::Angle z);
    psyqo::Matrix33& GetRotation();
    
    void ExtractFrustum(Frustum& frustum) const;

    int16_t GetAngleX() const { return m_angleX; }
    int16_t GetAngleY() const { return m_angleY; }
    int16_t GetAngleZ() const { return m_angleZ; }

  private:
    psyqo::Matrix33 m_rotationMatrix;
    psyqo::Trig<> m_trig;
    psyqo::Vec3 m_position;
    int16_t m_angleX = 0, m_angleY = 0, m_angleZ = 0;
};
}  // namespace psxsplash