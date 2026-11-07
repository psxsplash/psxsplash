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
    /// Sets the world-to-view matrix as given (a baked background's exact
    /// pose); the angles are what GetAngle* report.
    void SetRotationMatrix(const psyqo::Matrix33& m, int16_t angleX, int16_t angleY, int16_t angleZ) {
        m_rotationMatrix = m;
        m_angleX = angleX, m_angleY = angleY, m_angleZ = angleZ;
    }

    void SetProjectionH(int32_t h) { m_projH = h; }
    int32_t GetProjectionH() const { return m_projH; }

    void ExtractFrustum(Frustum& frustum) const;

    int16_t GetAngleX() const { return m_angleX; }
    int16_t GetAngleY() const { return m_angleY; }
    int16_t GetAngleZ() const { return m_angleZ; }

  private:
    psyqo::Matrix33 m_rotationMatrix;
    psyqo::Trig<> m_trig;
    psyqo::Vec3 m_position;
    int16_t m_angleX = 0, m_angleY = 0, m_angleZ = 0;
    int32_t m_projH = 120;
};
}  // namespace psxsplash