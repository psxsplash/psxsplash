#pragma once

#include <psyqo/advancedpad.hh>
#include <psyqo/trigonometry.hh>
#include <psyqo/vector.hh>
#include <psyqo/fixed-point.hh>

namespace psxsplash {

using namespace psyqo::fixed_point_literals;
using namespace psyqo::trig_literals;

class Controls {
  public:
    /// Force DualShock into analog mode
    /// Must be called BEFORE Init() since Init() hands SIO control to AdvancedPad.
    void forceAnalogMode();

    void Init();
    void HandleControls(psyqo::Vec3 &playerPosition, psyqo::Angle &playerRotationX, psyqo::Angle &playerRotationY,
                        psyqo::Angle &playerRotationZ, bool freecam, int32_t dt12);

    /// Update button state tracking - call before HandleControls
    void UpdateButtonStates();
    
    /// Set movement speeds from splashpack data (call once after scene load)
    void setMoveSpeed(psyqo::FixedPoint<12, uint16_t> speed) { m_moveSpeed.value = speed.value; }
    void setSprintSpeed(psyqo::FixedPoint<12, uint16_t> speed) { m_sprintSpeed.value = speed.value; }
    
    /// Check if a button was just pressed this frame
    bool wasButtonPressed(psyqo::AdvancedPad::Button button) const {
        uint16_t mask = 1u << static_cast<uint16_t>(button);
        return (m_currentButtons & mask) && !(m_previousButtons & mask);
    }
    
    /// Check if a button was just released this frame
    bool wasButtonReleased(psyqo::AdvancedPad::Button button) const {
        uint16_t mask = 1u << static_cast<uint16_t>(button);
        return !(m_currentButtons & mask) && (m_previousButtons & mask);
    }
    
    /// Check if a button is currently held
    bool isButtonHeld(psyqo::AdvancedPad::Button button) const {
        return m_input.isButtonPressed(psyqo::AdvancedPad::Pad::Pad1a, button);
    }
    
    /// Get bitmask of buttons pressed this frame
    uint16_t getButtonsPressed() const { return m_buttonsPressed; }
    
    /// Get bitmask of buttons released this frame
    uint16_t getButtonsReleased() const { return m_buttonsReleased; }

    /// Analog stick accessors (set during HandleControls)
    int16_t getLeftStickX() const { return m_leftStickX; }
    int16_t getLeftStickY() const { return m_leftStickY; }
    int16_t getRightStickX() const { return m_rightStickX; }
    int16_t getRightStickY() const { return m_rightStickY; }

    /// Set vibration motor values.
    /// @param smallMotor 0=off, non-zero=on (right/small motor, high frequency)
    /// @param largeMotor 0x00..0xFF speed (left/large motor, low frequency)
    void setMotors(uint8_t smallMotor, uint8_t largeMotor) {
        m_motorSmallCache = smallMotor;
        m_motorLargeCache = largeMotor;
    }

    /// Set only the small (right) vibration motor. 0=off, non-zero=on.
    void setSmallMotor(uint8_t value) {
        m_motorSmallCache = value;
    }

    /// Set only the large (left) vibration motor. 0x00..0xFF speed.
    void setLargeMotor(uint8_t value) {
        m_motorLargeCache = value;
    }

    /// Stop both vibration motors immediately.
    void stopMotors() {
        m_motorSmallCache = 0;
        m_motorLargeCache = 0;
    }

  private:
    psyqo::AdvancedPad m_input;
    psyqo::Trig<> m_trig;

    bool m_sprinting = false;
    static constexpr uint8_t m_stickDeadzone = 0x30;
    static constexpr psyqo::Angle rotSpeed = 0.01_pi;
    
    // Configurable movement speeds (set from splashpack, or defaults)
    psyqo::FixedPoint<12> m_moveSpeed = 0.002_fp;
    psyqo::FixedPoint<12> m_sprintSpeed = 0.01_fp;

    // Cached motor values for independent track control
    uint8_t m_motorSmallCache = 0;
    uint8_t m_motorLargeCache = 0;
    
    // Button state tracking
    uint16_t m_previousButtons = 0;
    uint16_t m_currentButtons = 0;
    uint16_t m_buttonsPressed = 0;
    uint16_t m_buttonsReleased = 0;
    
    // Analog stick values (centered at 0, range -127 to +127)
    int16_t m_leftStickX = 0;
    int16_t m_leftStickY = 0;
    int16_t m_rightStickX = 0;
    int16_t m_rightStickY = 0;
    
    /// Returns true if the connected pad is digital-only (no analog sticks)
    bool isDigitalPad() const;
    
    /// Get movement axes from D-pad as simulated stick values (-127 to +127)
    void getDpadAxes(int16_t &outX, int16_t &outY) const;

    /// Send cached motor values to the controller via raw SIO.
    /// Called each frame from UpdateButtonStates() after AdvancedPad's VSync poll.
    void sendMotorValues();
};

}  // namespace psxsplash