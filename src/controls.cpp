#include "controls.hh"

#include <psyqo/vector.hh>

void psxsplash::Controls::Init() { m_input.initialize(); }

void psxsplash::Controls::HandleControls(psyqo::Vec3 &playerPosition, psyqo::Angle &playerRotationX,
                                         psyqo::Angle &playerRotationY, psyqo::Angle &playerRotationZ, bool freecam,
                                         int deltaTime) {
    uint8_t rightX = m_input.getAdc(psyqo::AdvancedPad::Pad::Pad1a, 0);
    uint8_t rightY = m_input.getAdc(psyqo::AdvancedPad::Pad::Pad1a, 1);

    uint8_t leftX = m_input.getAdc(psyqo::AdvancedPad::Pad::Pad1a, 2);
    uint8_t leftY = m_input.getAdc(psyqo::AdvancedPad::Pad::Pad1a, 3);

    int16_t rightXOffset = (int16_t)rightX - 0x80;
    int16_t rightYOffset = (int16_t)rightY - 0x80;
    int16_t leftXOffset = (int16_t)leftX - 0x80;
    int16_t leftYOffset = (int16_t)leftY - 0x80;

    if (__builtin_abs(leftXOffset) < m_stickDeadzone && __builtin_abs(leftYOffset) < m_stickDeadzone) {
        m_sprinting = false;
    }

    if (m_input.isButtonPressed(psyqo::AdvancedPad::Pad::Pad1a, psyqo::AdvancedPad::Button::L3)) {
        m_sprinting = true;
    }

    psyqo::FixedPoint<12> speed = m_sprinting ? sprintSpeed : moveSpeed;

    if (__builtin_abs(rightXOffset) > m_stickDeadzone) {
        playerRotationY += (rightXOffset * rotSpeed * deltaTime) >> 7;
    }
    if (__builtin_abs(rightYOffset) > m_stickDeadzone) {
        playerRotationX -= (rightYOffset * rotSpeed * deltaTime) >> 7;
        playerRotationX = eastl::clamp(playerRotationX, -0.5_pi, 0.5_pi);
    }

    if (__builtin_abs(leftYOffset) > m_stickDeadzone) {
        psyqo::FixedPoint<12> forward = -(leftYOffset * speed * deltaTime) >> 7;
        playerPosition.x += m_trig.sin(playerRotationY) * forward;
        playerPosition.z += m_trig.cos(playerRotationY) * forward;
    }
    if (__builtin_abs(leftXOffset) > m_stickDeadzone) {
        psyqo::FixedPoint<12> strafe = -(leftXOffset * speed * deltaTime) >> 7;
        playerPosition.x -= m_trig.cos(playerRotationY) * strafe;
        playerPosition.z += m_trig.sin(playerRotationY) * strafe;
    }

    if (m_input.isButtonPressed(psyqo::AdvancedPad::Pad::Pad1a, psyqo::AdvancedPad::Button::L1)) {
        playerPosition.y += speed * deltaTime;
    }
    if (m_input.isButtonPressed(psyqo::AdvancedPad::Pad::Pad1a, psyqo::AdvancedPad::Button::R1)) {
        playerPosition.y -= speed * deltaTime;
    }
}