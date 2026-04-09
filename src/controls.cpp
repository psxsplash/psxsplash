#include "controls.hh"

#include <psyqo/hardware/cpu.hh>
#include <psyqo/hardware/sio.hh>
#include <psyqo/vector.hh>

namespace {

using namespace psyqo::Hardware;

void busyLoop(unsigned delay) {
    unsigned cycles = 0;
    while (++cycles < delay) asm("");
}

void flushRxBuffer() {
    while (SIO::Stat & SIO::Status::STAT_RXRDY) {
        SIO::Data.throwAway();
    }
}

uint8_t transceive(uint8_t dataOut) {
    SIO::Ctrl |= SIO::Control::CTRL_ERRRES;
    CPU::IReg.clear(CPU::IRQ::Controller);
    SIO::Data = dataOut;
    while (!(SIO::Stat & SIO::Status::STAT_RXRDY));
    return SIO::Data;
}

bool waitForAck() {
    int cyclesWaited = 0;
    static constexpr int ackTimeout = 0x137;
    while (!(CPU::IReg.isSet(CPU::IRQ::Controller)) && ++cyclesWaited < ackTimeout);
    if (cyclesWaited >= ackTimeout) return false;
    while (SIO::Stat & SIO::Status::STAT_ACK);  // Wait for ACK to go high
    return true;
}

void configurePort(uint8_t port) {
    SIO::Ctrl = (port * SIO::Control::CTRL_PORTSEL) | SIO::Control::CTRL_DTR;
    SIO::Baud = 0x88;
    flushRxBuffer();
    SIO::Ctrl |= (SIO::Control::CTRL_TXEN | SIO::Control::CTRL_ACKIRQEN);
    busyLoop(100);
}

// Send a command sequence to the pad and wait for ACK between each byte.
// Returns false if ACK was lost at any point.
bool sendCommand(const uint8_t *cmd, unsigned len) {
    for (unsigned i = 0; i < len; i++) {
        transceive(cmd[i]);
        if (i < len - 1) {
            if (!waitForAck()) return false;
        }
    }
    return true;
}

}  // namespace

void psxsplash::Controls::forceAnalogMode() {
    // Initialize SIO for pad communication
    using namespace psyqo::Hardware;
    SIO::Ctrl = SIO::Control::CTRL_IR;
    SIO::Baud = 0x88;
    SIO::Mode = 0xd;
    SIO::Ctrl = 0;

    // Sequence for port 0 (Pad 1):
    // 1) Enter config mode
    static const uint8_t enterConfig[] = {0x01, 0x43, 0x00, 0x01, 0x00};
    // 2) Set analog mode (0x01) + lock (0x03)
    static const uint8_t setAnalog[] = {0x01, 0x44, 0x00, 0x01, 0x03, 0x00, 0x00, 0x00, 0x00};
    // 3) Map vibration motors: byte[0]=small motor (M2), byte[1]=large motor (M1)
    static const uint8_t mapMotors[] = {0x01, 0x4D, 0x00, 0x00, 0x01, 0xFF, 0xFF, 0xFF, 0xFF};
    // 4) Exit config mode
    static const uint8_t exitConfig[] = {0x01, 0x43, 0x00, 0x00, 0x00};

    configurePort(0);
    sendCommand(enterConfig, sizeof(enterConfig));
    SIO::Ctrl = 0;

    configurePort(0);
    sendCommand(setAnalog, sizeof(setAnalog));
    SIO::Ctrl = 0;

    configurePort(0);
    sendCommand(mapMotors, sizeof(mapMotors));
    SIO::Ctrl = 0;

    configurePort(0);
    sendCommand(exitConfig, sizeof(exitConfig));
    SIO::Ctrl = 0;
}

void psxsplash::Controls::Init() { m_input.initialize(); }

bool psxsplash::Controls::isDigitalPad() const {
    uint8_t padType = m_input.getPadType(psyqo::AdvancedPad::Pad::Pad1a);
    // Digital pad (0x41) has no analog sticks
    // Also treat disconnected pads as digital (D-pad still works through button API)
    return padType == psyqo::AdvancedPad::PadType::DigitalPad ||
           padType == psyqo::AdvancedPad::PadType::None;
}

void psxsplash::Controls::getDpadAxes(int16_t &outX, int16_t &outY) const {
    outX = 0;
    outY = 0;
    // D-pad produces full-magnitude values (like pushing the stick to the edge)
    if (m_input.isButtonPressed(psyqo::AdvancedPad::Pad::Pad1a, psyqo::AdvancedPad::Button::Up))
        outY = -127;
    if (m_input.isButtonPressed(psyqo::AdvancedPad::Pad::Pad1a, psyqo::AdvancedPad::Button::Down))
        outY = 127;
    if (m_input.isButtonPressed(psyqo::AdvancedPad::Pad::Pad1a, psyqo::AdvancedPad::Button::Left))
        outX = -127;
    if (m_input.isButtonPressed(psyqo::AdvancedPad::Pad::Pad1a, psyqo::AdvancedPad::Button::Right))
        outX = 127;
}

void psxsplash::Controls::UpdateButtonStates() {
    m_previousButtons = m_currentButtons;

    // Send motor values via raw SIO (after AdvancedPad's VSync poll has completed)
    sendMotorValues();
    
    // Read all button states into a single bitmask
    m_currentButtons = 0;
    if (m_input.isButtonPressed(psyqo::AdvancedPad::Pad::Pad1a, psyqo::AdvancedPad::Button::Cross))    m_currentButtons |= (1u << psyqo::AdvancedPad::Button::Cross);
    if (m_input.isButtonPressed(psyqo::AdvancedPad::Pad::Pad1a, psyqo::AdvancedPad::Button::Circle))   m_currentButtons |= (1u << psyqo::AdvancedPad::Button::Circle);
    if (m_input.isButtonPressed(psyqo::AdvancedPad::Pad::Pad1a, psyqo::AdvancedPad::Button::Square))   m_currentButtons |= (1u << psyqo::AdvancedPad::Button::Square);
    if (m_input.isButtonPressed(psyqo::AdvancedPad::Pad::Pad1a, psyqo::AdvancedPad::Button::Triangle)) m_currentButtons |= (1u << psyqo::AdvancedPad::Button::Triangle);
    if (m_input.isButtonPressed(psyqo::AdvancedPad::Pad::Pad1a, psyqo::AdvancedPad::Button::L1))       m_currentButtons |= (1u << psyqo::AdvancedPad::Button::L1);
    if (m_input.isButtonPressed(psyqo::AdvancedPad::Pad::Pad1a, psyqo::AdvancedPad::Button::L2))       m_currentButtons |= (1u << psyqo::AdvancedPad::Button::L2);
    if (m_input.isButtonPressed(psyqo::AdvancedPad::Pad::Pad1a, psyqo::AdvancedPad::Button::L3))       m_currentButtons |= (1u << psyqo::AdvancedPad::Button::L3);
    if (m_input.isButtonPressed(psyqo::AdvancedPad::Pad::Pad1a, psyqo::AdvancedPad::Button::R1))       m_currentButtons |= (1u << psyqo::AdvancedPad::Button::R1);
    if (m_input.isButtonPressed(psyqo::AdvancedPad::Pad::Pad1a, psyqo::AdvancedPad::Button::R2))       m_currentButtons |= (1u << psyqo::AdvancedPad::Button::R2);
    if (m_input.isButtonPressed(psyqo::AdvancedPad::Pad::Pad1a, psyqo::AdvancedPad::Button::R3))       m_currentButtons |= (1u << psyqo::AdvancedPad::Button::R3);
    if (m_input.isButtonPressed(psyqo::AdvancedPad::Pad::Pad1a, psyqo::AdvancedPad::Button::Start))    m_currentButtons |= (1u << psyqo::AdvancedPad::Button::Start);
    if (m_input.isButtonPressed(psyqo::AdvancedPad::Pad::Pad1a, psyqo::AdvancedPad::Button::Select))   m_currentButtons |= (1u << psyqo::AdvancedPad::Button::Select);
    if (m_input.isButtonPressed(psyqo::AdvancedPad::Pad::Pad1a, psyqo::AdvancedPad::Button::Up))       m_currentButtons |= (1u << psyqo::AdvancedPad::Button::Up);
    if (m_input.isButtonPressed(psyqo::AdvancedPad::Pad::Pad1a, psyqo::AdvancedPad::Button::Down))     m_currentButtons |= (1u << psyqo::AdvancedPad::Button::Down);
    if (m_input.isButtonPressed(psyqo::AdvancedPad::Pad::Pad1a, psyqo::AdvancedPad::Button::Left))     m_currentButtons |= (1u << psyqo::AdvancedPad::Button::Left);
    if (m_input.isButtonPressed(psyqo::AdvancedPad::Pad::Pad1a, psyqo::AdvancedPad::Button::Right))    m_currentButtons |= (1u << psyqo::AdvancedPad::Button::Right);
    
    // Calculate pressed and released buttons
    m_buttonsPressed = m_currentButtons & ~m_previousButtons;
    m_buttonsReleased = m_previousButtons & ~m_currentButtons;
}

void psxsplash::Controls::HandleControls(psyqo::Vec3 &playerPosition, psyqo::Angle &playerRotationX,
                                         psyqo::Angle &playerRotationY, psyqo::Angle &playerRotationZ, bool freecam,
                                         int32_t dt12) {
    bool digital = isDigitalPad();
    
    int16_t rightXOffset, rightYOffset, leftXOffset, leftYOffset;
    
    if (digital) {
        // Digital pad: use D-pad for movement, L1/R1 for rotation
        getDpadAxes(leftXOffset, leftYOffset);
        // L1/R1 for horizontal look rotation (no vertical on digital)
        rightXOffset = 0;
        rightYOffset = 0;
        if (m_input.isButtonPressed(psyqo::AdvancedPad::Pad::Pad1a, psyqo::AdvancedPad::Button::R1))
            rightXOffset = 90;
        if (m_input.isButtonPressed(psyqo::AdvancedPad::Pad::Pad1a, psyqo::AdvancedPad::Button::L1))
            rightXOffset = -90;
    } else {
        // Analog pad: read stick ADC values
        uint8_t rightX = m_input.getAdc(psyqo::AdvancedPad::Pad::Pad1a, 0);
        uint8_t rightY = m_input.getAdc(psyqo::AdvancedPad::Pad::Pad1a, 1);
        uint8_t leftX = m_input.getAdc(psyqo::AdvancedPad::Pad::Pad1a, 2);
        uint8_t leftY = m_input.getAdc(psyqo::AdvancedPad::Pad::Pad1a, 3);

        rightXOffset = (int16_t)rightX - 0x80;
        rightYOffset = (int16_t)rightY - 0x80;
        leftXOffset = (int16_t)leftX - 0x80;
        leftYOffset = (int16_t)leftY - 0x80;
        
        // On analog pad, also check D-pad as fallback (when sticks are centered)
        if (__builtin_abs(leftXOffset) < m_stickDeadzone && __builtin_abs(leftYOffset) < m_stickDeadzone) {
            int16_t dpadX, dpadY;
            getDpadAxes(dpadX, dpadY);
            if (dpadX != 0 || dpadY != 0) {
                leftXOffset = dpadX;
                leftYOffset = dpadY;
            }
        }
    }

    // Sprint toggle (L3 for analog, Square for digital)
    if (__builtin_abs(leftXOffset) < m_stickDeadzone && __builtin_abs(leftYOffset) < m_stickDeadzone) {
        m_sprinting = false;
    }

    // Store final stick values for Lua API access
    m_leftStickX = leftXOffset;
    m_leftStickY = leftYOffset;
    m_rightStickX = rightXOffset;
    m_rightStickY = rightYOffset;

    if (digital) {
        if (m_input.isButtonPressed(psyqo::AdvancedPad::Pad::Pad1a, psyqo::AdvancedPad::Button::Square)) {
            m_sprinting = true;
        }
    } else {
        if (m_input.isButtonPressed(psyqo::AdvancedPad::Pad::Pad1a, psyqo::AdvancedPad::Button::L3)) {
            m_sprinting = true;
        }
    }

    psyqo::FixedPoint<12> speed = m_sprinting ? m_sprintSpeed : m_moveSpeed;

    // dt12 is 4.12 fixed-point: 4096 = one 30fps frame.
    // All motion scaling uses (expr * dt12) >> 12 to replace the old integer multiply.

    // Rotation (right stick or L1/R1)
    if (__builtin_abs(rightXOffset) > m_stickDeadzone) {
        psyqo::Angle rotDelta = (rightXOffset * rotSpeed) >> 7;
        rotDelta.value = (int32_t)(((int64_t)rotDelta.value * dt12) >> 12);
        playerRotationY += rotDelta;
    }
    if (__builtin_abs(rightYOffset) > m_stickDeadzone) {
        psyqo::Angle rotDelta = (rightYOffset * rotSpeed) >> 7;
        rotDelta.value = (int32_t)(((int64_t)rotDelta.value * dt12) >> 12);
        playerRotationX -= rotDelta;
        playerRotationX = eastl::clamp(playerRotationX, -0.5_pi, 0.5_pi);
    }

    // Movement (left stick or D-pad)
    if (__builtin_abs(leftYOffset) > m_stickDeadzone) {
        psyqo::FixedPoint<12> forward = -(leftYOffset * speed) >> 7;
        forward.value = (int32_t)(((int64_t)forward.value * dt12) >> 12);
        playerPosition.x += m_trig.sin(playerRotationY) * forward;
        playerPosition.z += m_trig.cos(playerRotationY) * forward;
    }
    if (__builtin_abs(leftXOffset) > m_stickDeadzone) {
        psyqo::FixedPoint<12> strafe = -(leftXOffset * speed) >> 7;
        strafe.value = (int32_t)(((int64_t)strafe.value * dt12) >> 12);
        playerPosition.x -= m_trig.cos(playerRotationY) * strafe;
        playerPosition.z += m_trig.sin(playerRotationY) * strafe;
    }

    if (freecam) {
        psyqo::FixedPoint<12> dtSpeed;
        dtSpeed.value = (int32_t)(((int64_t)speed.value * dt12) >> 12);
        if (m_input.isButtonPressed(psyqo::AdvancedPad::Pad::Pad1a, psyqo::AdvancedPad::Button::L1)) {
            playerPosition.y += dtSpeed;
        }
        if (m_input.isButtonPressed(psyqo::AdvancedPad::Pad::Pad1a, psyqo::AdvancedPad::Button::R1)) {
            playerPosition.y -= dtSpeed;
        }
    }
}

void psxsplash::Controls::sendMotorValues() {
    // Skip SIO transaction when both motors are off — nothing to send.
    if (m_motorSmallCache == 0 && m_motorLargeCache == 0) return;

    using namespace psyqo::Hardware;

    // Send a 0x42 ReadPad command with motor bytes via raw SIO.
    // AdvancedPad's VSync poll sends 0x00 for the motor bytes, so we
    // re-send the desired values here during the game tick.  Motor inertia
    // bridges the brief gap where AdvancedPad zeroes them.
    configurePort(0);

    transceive(0x01);         // byte 0: device select
    if (!waitForAck()) { SIO::Ctrl = 0; return; }

    transceive(0x42);         // byte 1: ReadPad command
    if (!waitForAck()) { SIO::Ctrl = 0; return; }

    transceive(0x00);         // byte 2: TAP (ignored)
    if (!waitForAck()) { SIO::Ctrl = 0; return; }

    transceive(m_motorSmallCache);  // byte 3: small motor (right, on/off)
    if (!waitForAck()) { SIO::Ctrl = 0; return; }

    transceive(m_motorLargeCache);  // byte 4: large motor (left, 0-255)
    // No more bytes needed; remaining response bytes are button/analog data
    // which we don't care about (AdvancedPad already reads them).

    SIO::Ctrl = 0;
}