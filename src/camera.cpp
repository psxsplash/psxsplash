#include "camera.hh"

#include <psyqo/fixed-point.hh>
#include <psyqo/matrix.hh>
#include <psyqo/soft-math.hh>
#include <psyqo/trigonometry.hh>

psxsplash::Camera::Camera() {
    m_rotationMatrix = psyqo::SoftMath::generateRotationMatrix33(0, psyqo::SoftMath::Axis::X, m_trig);
}

void psxsplash::Camera::MoveX(psyqo::FixedPoint<12> x) { m_position.x += x; }

void psxsplash::Camera::MoveY(psyqo::FixedPoint<12> y) { m_position.y += y; }

void psxsplash::Camera::MoveZ(psyqo::FixedPoint<12> z) { m_position.z += z; }

void psxsplash::Camera::SetPosition(psyqo::FixedPoint<12> x, psyqo::FixedPoint<12> y, psyqo::FixedPoint<12> z) {
    m_position.x = x;
    m_position.y = y;
    m_position.z = z;
}

void psxsplash::Camera::SetRotation(psyqo::Angle x, psyqo::Angle y, psyqo::Angle z) {
    m_angleX = (int16_t)x.value;
    m_angleY = (int16_t)y.value;
    m_angleZ = (int16_t)z.value;

    auto rotX = psyqo::SoftMath::generateRotationMatrix33(x, psyqo::SoftMath::Axis::X, m_trig);
    auto rotY = psyqo::SoftMath::generateRotationMatrix33(y, psyqo::SoftMath::Axis::Y, m_trig);
    auto rotZ = psyqo::SoftMath::generateRotationMatrix33(z, psyqo::SoftMath::Axis::Z, m_trig);

    psyqo::SoftMath::multiplyMatrix33(rotY, rotX, &rotY);
    psyqo::SoftMath::multiplyMatrix33(rotY, rotZ, &rotY);

    m_rotationMatrix = rotY;
}

psyqo::Matrix33& psxsplash::Camera::GetRotation() { return m_rotationMatrix; }

void psxsplash::Camera::ExtractFrustum(Frustum& frustum) const {

    constexpr int32_t SCREEN_HALF_WIDTH = 160;  
    constexpr int32_t SCREEN_HALF_HEIGHT = 120;  
    constexpr int32_t H = 120;                  
    
    int32_t rightX = m_rotationMatrix.vs[0].x.raw();
    int32_t rightY = m_rotationMatrix.vs[0].y.raw();
    int32_t rightZ = m_rotationMatrix.vs[0].z.raw();
    
    int32_t upX = m_rotationMatrix.vs[1].x.raw();
    int32_t upY = m_rotationMatrix.vs[1].y.raw();
    int32_t upZ = m_rotationMatrix.vs[1].z.raw();
    
    int32_t fwdX = m_rotationMatrix.vs[2].x.raw();
    int32_t fwdY = m_rotationMatrix.vs[2].y.raw();
    int32_t fwdZ = m_rotationMatrix.vs[2].z.raw();
    
    int32_t camX = m_position.x.raw();
    int32_t camY = m_position.y.raw();
    int32_t camZ = m_position.z.raw();
    
    frustum.planes[0].nx = fwdX;
    frustum.planes[0].ny = fwdY;
    frustum.planes[0].nz = fwdZ;
    int64_t fwdDotCam = ((int64_t)fwdX * camX + (int64_t)fwdY * camY + (int64_t)fwdZ * camZ) >> 12;
    frustum.planes[0].d = -fwdDotCam;
    
    frustum.planes[1].nx = -fwdX;
    frustum.planes[1].ny = -fwdY;
    frustum.planes[1].nz = -fwdZ;
    frustum.planes[1].d = fwdDotCam + (4096 * 2000);
    
    frustum.planes[2].nx = ((int64_t)rightX * H + (int64_t)fwdX * SCREEN_HALF_WIDTH) >> 12;
    frustum.planes[2].ny = ((int64_t)rightY * H + (int64_t)fwdY * SCREEN_HALF_WIDTH) >> 12;
    frustum.planes[2].nz = ((int64_t)rightZ * H + (int64_t)fwdZ * SCREEN_HALF_WIDTH) >> 12;
    frustum.planes[2].d = -(((int64_t)frustum.planes[2].nx * camX +
                             (int64_t)frustum.planes[2].ny * camY +
                             (int64_t)frustum.planes[2].nz * camZ) >> 12);
    
    frustum.planes[3].nx = ((int64_t)(-rightX) * H + (int64_t)fwdX * SCREEN_HALF_WIDTH) >> 12;
    frustum.planes[3].ny = ((int64_t)(-rightY) * H + (int64_t)fwdY * SCREEN_HALF_WIDTH) >> 12;
    frustum.planes[3].nz = ((int64_t)(-rightZ) * H + (int64_t)fwdZ * SCREEN_HALF_WIDTH) >> 12;
    frustum.planes[3].d = -(((int64_t)frustum.planes[3].nx * camX +
                             (int64_t)frustum.planes[3].ny * camY +
                             (int64_t)frustum.planes[3].nz * camZ) >> 12);

    frustum.planes[4].nx = ((int64_t)upX * H + (int64_t)fwdX * SCREEN_HALF_HEIGHT) >> 12;
    frustum.planes[4].ny = ((int64_t)upY * H + (int64_t)fwdY * SCREEN_HALF_HEIGHT) >> 12;
    frustum.planes[4].nz = ((int64_t)upZ * H + (int64_t)fwdZ * SCREEN_HALF_HEIGHT) >> 12;
    frustum.planes[4].d = -(((int64_t)frustum.planes[4].nx * camX +
                             (int64_t)frustum.planes[4].ny * camY +
                             (int64_t)frustum.planes[4].nz * camZ) >> 12);
    
    frustum.planes[5].nx = ((int64_t)(-upX) * H + (int64_t)fwdX * SCREEN_HALF_HEIGHT) >> 12;
    frustum.planes[5].ny = ((int64_t)(-upY) * H + (int64_t)fwdY * SCREEN_HALF_HEIGHT) >> 12;
    frustum.planes[5].nz = ((int64_t)(-upZ) * H + (int64_t)fwdZ * SCREEN_HALF_HEIGHT) >> 12;
    frustum.planes[5].d = -(((int64_t)frustum.planes[5].nx * camX +
                             (int64_t)frustum.planes[5].ny * camY +
                             (int64_t)frustum.planes[5].nz * camZ) >> 12);
}