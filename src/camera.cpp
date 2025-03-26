#include "camera.hh"

#include <psyqo/fixed-point.hh>
#include <psyqo/matrix.hh>
#include <psyqo/soft-math.hh>
#include <psyqo/trigonometry.hh>

psxsplash::Camera::Camera() {
    m_rotationMatrix = psyqo::SoftMath::generateRotationMatrix33(0, psyqo::SoftMath::Axis::X, m_trig);
}

void psxsplash::Camera::moveX(psyqo::FixedPoint<12> x) { m_rotation.x += -x; }

void psxsplash::Camera::moveY(psyqo::FixedPoint<12> y) { m_rotation.y += -y; }

void psxsplash::Camera::moveZ(psyqo::FixedPoint<12> z) { m_rotation.z += -z; }

void psxsplash::Camera::setPosition(psyqo::FixedPoint<12> x, psyqo::FixedPoint<12> y, psyqo::FixedPoint<12> z) {
    m_rotation.x = -x;
    m_rotation.y = -y;
    m_rotation.z = -z;
}

void psxsplash::Camera::rotateX(psyqo::Angle x) {
    auto rot = psyqo::SoftMath::generateRotationMatrix33(-x, psyqo::SoftMath::Axis::X, m_trig);

    psyqo::SoftMath::multiplyMatrix33(m_rotationMatrix, rot, &m_rotationMatrix);
}

void psxsplash::Camera::rotateY(psyqo::Angle y) {
    auto rot = psyqo::SoftMath::generateRotationMatrix33(-y, psyqo::SoftMath::Axis::Y, m_trig);

    psyqo::SoftMath::multiplyMatrix33(m_rotationMatrix, rot, &m_rotationMatrix);
}

void psxsplash::Camera::rotateZ(psyqo::Angle z) {
    auto rot = psyqo::SoftMath::generateRotationMatrix33(-z, psyqo::SoftMath::Axis::Y, m_trig);

    psyqo::SoftMath::multiplyMatrix33(m_rotationMatrix, rot, &m_rotationMatrix);
}

void psxsplash::Camera::setRotation(psyqo::Angle x, psyqo::Angle y, psyqo::Angle z) {
    auto rotX = psyqo::SoftMath::generateRotationMatrix33(x, psyqo::SoftMath::Axis::X, m_trig);
    auto rotY = psyqo::SoftMath::generateRotationMatrix33(y, psyqo::SoftMath::Axis::Y, m_trig);
    auto rotZ = psyqo::SoftMath::generateRotationMatrix33(z, psyqo::SoftMath::Axis::Z, m_trig);

    psyqo::SoftMath::multiplyMatrix33(rotY, rotX, &rotY);

    psyqo::SoftMath::multiplyMatrix33(rotY, rotZ, &rotY);
    m_rotationMatrix = rotY;
}

psyqo::Matrix33& psxsplash::Camera::getRotation() { return m_rotationMatrix; }