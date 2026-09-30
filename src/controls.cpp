#include "controls.hh"

#include <psyqo/hardware/cpu.hh>
#include <psyqo/hardware/sio.hh>
#include <psyqo/vector.hh>

#include "irqack.hh"

namespace {

using namespace psyqo::Hardware;

void busyLoop(unsigned delay) {
    unsigned cycles = 0;
    while (++cycles < delay) asm("");
}

// NO WAIT ON THIS PORT MAY BE UNBOUNDED. THE CONSOLE IS THE THING AT STAKE.
//
// Every loop below used to spin on a hardware flag with no way out, and one of
// them is how a real console froze: `while (!(SIO::Stat & STAT_RXRDY));` after
// writing DATA, waiting for a byte from a pad that had already given up.
//
// A PlayStation pad ABORTS a transfer if the console does not clock the next byte
// promptly after ACK, and stops answering until DTR is re-asserted. That is not
// hypothetical here -- under the retail BIOS an SIO1 receive interrupt costs on
// the order of 150us to dispatch, and this game holds a live serial link, so one
// landing between two pad bytes is enough. The next wait then never returns: the
// last frame stays on screen, interrupts are still enabled, and installCrashHandler
// says nothing because this is not an exception. It is indistinguishable from a
// dead console, which is what makes it so expensive to diagnose -- it destroys the
// evidence for every other bug at the same time.
//
// So the waits are bounded and failure is REPORTED. Every caller already has a
// give-up path (sendCommand returns false, forceAnalogMode treats a missing pad as
// skipped harmlessly), because a controller that is simply not plugged in has
// always had to be survivable. Timing out now takes that same path.
//
// The budget: a byte at SIO0's 250kHz is 32us, and one iteration here is a single
// I/O read, roughly 0.4us. 1024 iterations is ~450us -- more than ten byte-times,
// so a healthy pad never comes close, and a dead one costs half a millisecond
// instead of the machine.
constexpr unsigned c_sioSpinLimit = 1024;

void flushRxBuffer() {
    unsigned spin = c_sioSpinLimit;
    while ((SIO::Stat & SIO::Status::STAT_RXRDY) && spin--) {
        SIO::Data.throwAway();
    }
}

// Acknowledge the CONTROLLER interrupt without collateral damage.
//
// `IReg.clear()` is a read-modify-write on I_STAT (hwregs.hh, operator&=), and
// I_STAT is acknowledge-by-writing-ZERO - so it writes a zero back into every bit
// that was not pending at the moment it read. If the SIO1 receive interrupt asserts
// inside that window, this call acknowledges it by accident, and because
// SIO_STAT.9 is sticky while I_STAT.8 is edge-triggered, no further edge is ever
// generated: the link's receive interrupt is dead for the rest of the session. See
// Sio1::rearmRx() for the full mechanism and the hardware references.
//
// psyqo's AdvancedPad does the same thing and cannot be edited here, which is why
// Sio1::rearmRx() exists to undo it. But there is no reason for THIS driver to be a
// second source of the same fault: with interrupts off, nothing can assert between
// the read and the write, so the window closes completely.
//
// The critical section this used to hold is gone, and so is the MAIN THREAD ONLY
// restriction that came with it: `ackIrq` writes I_STAT without reading it, so there
// is no read-modify-write to protect. `I_STAT = ~bit` is "acknowledge this bit,
// leave every other pending", which is what the critical section was buying at the
// cost of disabling interrupts and of being unusable from a handler. Immune by
// construction rather than by timing, and one store instead of three. See irqack.hh.
inline void ackControllerIrq() { psxsplash::ackIrq(CPU::IRQ::Controller); }

// Returns false if the pad never answered; *dataIn is untouched in that case.
bool transceive(uint8_t dataOut, uint8_t *dataIn = nullptr) {
    SIO::Ctrl |= SIO::Control::CTRL_ERRRES;
    ackControllerIrq();
    SIO::Data = dataOut;
    unsigned spin = c_sioSpinLimit;
    while (!(SIO::Stat & SIO::Status::STAT_RXRDY) && spin--);
    if (!(SIO::Stat & SIO::Status::STAT_RXRDY)) return false;
    uint8_t b = SIO::Data;
    if (dataIn) *dataIn = b;
    return true;
}

// Polls the Controller IRQ FLAG rather than taking the interrupt, which makes it
// a deliberate race with whoever owns that IRQ - psyqo's AdvancedPad and
// MemoryCard both clear it in their handlers. If they win, this times out.
//
// That is survivable, and it is why the timeout exists rather than a bare spin:
// the only callers are forceAnalogMode() and the rumble writes, both of which are
// best-effort. Per-frame input does NOT come through here - it comes from
// psyqo::AdvancedPad, which is takeover-aware. So the worst case is that analog
// mode is not forced or a motor does not buzz, never a lost button press.
//
// Taking over the kernel makes psyqo's dispatch faster and so makes losing this
// race MORE likely, not less. If analog mode or rumble ever stops working, this
// is the first place to look - and the fix is to stop polling the shared flag,
// not to lengthen the timeout.
bool waitForAck() {
    int cyclesWaited = 0;
    static constexpr int ackTimeout = 0x137;
    while (!(CPU::IReg.isSet(CPU::IRQ::Controller)) && ++cyclesWaited < ackTimeout);
    if (cyclesWaited >= ackTimeout) return false;
    // Wait for ACK to go high -- BOUNDED, see c_sioSpinLimit. A pad holding ACK
    // low forever is precisely the wedged-peripheral case that must not take the
    // console with it.
    unsigned spin = c_sioSpinLimit;
    while ((SIO::Stat & SIO::Status::STAT_ACK) && spin--);
    return !(SIO::Stat & SIO::Status::STAT_ACK);
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
        // A timed-out byte now aborts the command instead of spinning forever.
        // The caller already treats a failed command as "no pad here", which is
        // the right reading: a pad that stopped answering mid-sequence is, for
        // this purpose, indistinguishable from one that was never plugged in.
        if (!transceive(cmd[i])) return false;
        if (i < len - 1) {
            if (!waitForAck()) return false;
        }
    }
    return true;
}

}  // namespace

uint32_t psxsplash::Controls::s_padInitCount = 0;

void psxsplash::Controls::forceAnalogMode() {
    // Once per boot. The pad latches analog mode itself, so a scene change does
    // not undo it -- and this is four raw SIO0 command chains per port, every byte
    // of which is a busy-wait. See the header.
    if (m_analogForced) return;
    m_analogForced = true;

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

    // Configure both ports: a controller in port 1 (player 1) and/or port 2
    // (player 2). A missing controller simply loses ACK and is skipped harmlessly.
    for (uint8_t port = 0; port < 2; ++port) {
        configurePort(port);
        sendCommand(enterConfig, sizeof(enterConfig));
        SIO::Ctrl = 0;

        configurePort(port);
        sendCommand(setAnalog, sizeof(setAnalog));
        SIO::Ctrl = 0;

        configurePort(port);
        sendCommand(mapMotors, sizeof(mapMotors));
        SIO::Ctrl = 0;

        configurePort(port);
        sendCommand(exitConfig, sizeof(exitConfig));
        SIO::Ctrl = 0;
    }
}

// ONCE PER BOOT. psyqo::AdvancedPad::initialize() appends a readPad() callback to
// the kernel's per-frame list and nothing can ever remove it, so calling this from
// every scene load leaked two blocking SIO0 pad polls per frame, permanently, per
// transition -- and that is what turned a missing timeout into a hard freeze on
// hardware. The full reasoning is on the declaration in controls.hh; it is written
// there because that is where somebody deleting this guard will be looking.
void psxsplash::Controls::Init() {
    if (m_padInitialized) return;
    m_padInitialized = true;
    s_padInitCount++;
    m_input.initialize();
}

bool psxsplash::Controls::isDigitalPad() const {
    uint8_t padType = m_input.getPadType(m_pad);
    // Digital pad (0x41) has no analog sticks
    // Also treat disconnected pads as digital (D-pad still works through button API)
    return padType == psyqo::AdvancedPad::PadType::DigitalPad ||
           padType == psyqo::AdvancedPad::PadType::None;
}

void psxsplash::Controls::getDpadAxes(int16_t &outX, int16_t &outY) const {
    outX = 0;
    outY = 0;
    // D-pad produces full-magnitude values (like pushing the stick to the edge)
    if (m_input.isButtonPressed(m_pad, psyqo::AdvancedPad::Button::Up))
        outY = -127;
    if (m_input.isButtonPressed(m_pad, psyqo::AdvancedPad::Button::Down))
        outY = 127;
    if (m_input.isButtonPressed(m_pad, psyqo::AdvancedPad::Button::Left))
        outX = -127;
    if (m_input.isButtonPressed(m_pad, psyqo::AdvancedPad::Button::Right))
        outX = 127;
}

void psxsplash::Controls::UpdateButtonStatesPlayer1() {
    m_pad = psyqo::AdvancedPad::Pad::Pad1a;
    m_port = 0;
    updateButtonStates();
}

void psxsplash::Controls::UpdateButtonStatesPlayer2() {
    m_pad = psyqo::AdvancedPad::Pad::Pad2a;
    m_port = 1;
    updateButtonStates();
}

void psxsplash::Controls::updateButtonStates() {
    m_previousButtons = m_currentButtons;

    // Send motor values via raw SIO (after AdvancedPad's VSync poll has completed)
    sendMotorValues();
    
    // Read all button states into a single bitmask
    m_currentButtons = 0;
    if (m_input.isButtonPressed(m_pad, psyqo::AdvancedPad::Button::Cross))    m_currentButtons |= (1u << psyqo::AdvancedPad::Button::Cross);
    if (m_input.isButtonPressed(m_pad, psyqo::AdvancedPad::Button::Circle))   m_currentButtons |= (1u << psyqo::AdvancedPad::Button::Circle);
    if (m_input.isButtonPressed(m_pad, psyqo::AdvancedPad::Button::Square))   m_currentButtons |= (1u << psyqo::AdvancedPad::Button::Square);
    if (m_input.isButtonPressed(m_pad, psyqo::AdvancedPad::Button::Triangle)) m_currentButtons |= (1u << psyqo::AdvancedPad::Button::Triangle);
    if (m_input.isButtonPressed(m_pad, psyqo::AdvancedPad::Button::L1))       m_currentButtons |= (1u << psyqo::AdvancedPad::Button::L1);
    if (m_input.isButtonPressed(m_pad, psyqo::AdvancedPad::Button::L2))       m_currentButtons |= (1u << psyqo::AdvancedPad::Button::L2);
    if (m_input.isButtonPressed(m_pad, psyqo::AdvancedPad::Button::L3))       m_currentButtons |= (1u << psyqo::AdvancedPad::Button::L3);
    if (m_input.isButtonPressed(m_pad, psyqo::AdvancedPad::Button::R1))       m_currentButtons |= (1u << psyqo::AdvancedPad::Button::R1);
    if (m_input.isButtonPressed(m_pad, psyqo::AdvancedPad::Button::R2))       m_currentButtons |= (1u << psyqo::AdvancedPad::Button::R2);
    if (m_input.isButtonPressed(m_pad, psyqo::AdvancedPad::Button::R3))       m_currentButtons |= (1u << psyqo::AdvancedPad::Button::R3);
    if (m_input.isButtonPressed(m_pad, psyqo::AdvancedPad::Button::Start))    m_currentButtons |= (1u << psyqo::AdvancedPad::Button::Start);
    if (m_input.isButtonPressed(m_pad, psyqo::AdvancedPad::Button::Select))   m_currentButtons |= (1u << psyqo::AdvancedPad::Button::Select);
    if (m_input.isButtonPressed(m_pad, psyqo::AdvancedPad::Button::Up))       m_currentButtons |= (1u << psyqo::AdvancedPad::Button::Up);
    if (m_input.isButtonPressed(m_pad, psyqo::AdvancedPad::Button::Down))     m_currentButtons |= (1u << psyqo::AdvancedPad::Button::Down);
    if (m_input.isButtonPressed(m_pad, psyqo::AdvancedPad::Button::Left))     m_currentButtons |= (1u << psyqo::AdvancedPad::Button::Left);
    if (m_input.isButtonPressed(m_pad, psyqo::AdvancedPad::Button::Right))    m_currentButtons |= (1u << psyqo::AdvancedPad::Button::Right);
    
    // Calculate pressed and released buttons
    m_buttonsPressed = m_currentButtons & ~m_previousButtons;
    m_buttonsReleased = m_previousButtons & ~m_currentButtons;
}

void psxsplash::Controls::HandleControlsPlayer1(psyqo::Vec3 &playerPosition, psyqo::Angle &playerRotationX,
                                                psyqo::Angle &playerRotationY, psyqo::Angle &playerRotationZ,
                                                bool freecam, int32_t dt12) {
    m_pad = psyqo::AdvancedPad::Pad::Pad1a;
    m_port = 0;
    handleControls(playerPosition, playerRotationX, playerRotationY, playerRotationZ, freecam, dt12);
}

void psxsplash::Controls::HandleControlsPlayer2(psyqo::Vec3 &playerPosition, psyqo::Angle &playerRotationX,
                                                psyqo::Angle &playerRotationY, psyqo::Angle &playerRotationZ,
                                                bool freecam, int32_t dt12) {
    m_pad = psyqo::AdvancedPad::Pad::Pad2a;
    m_port = 1;
    handleControls(playerPosition, playerRotationX, playerRotationY, playerRotationZ, freecam, dt12);
}

void psxsplash::Controls::handleControls(psyqo::Vec3 &playerPosition, psyqo::Angle &playerRotationX,
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
        if (m_input.isButtonPressed(m_pad, psyqo::AdvancedPad::Button::R1))
            rightXOffset = 90;
        if (m_input.isButtonPressed(m_pad, psyqo::AdvancedPad::Button::L1))
            rightXOffset = -90;
    } else {
        // Analog pad: read stick ADC values
        uint8_t rightX = m_input.getAdc(m_pad, 0);
        uint8_t rightY = m_input.getAdc(m_pad, 1);
        uint8_t leftX = m_input.getAdc(m_pad, 2);
        uint8_t leftY = m_input.getAdc(m_pad, 3);

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
        if (m_input.isButtonPressed(m_pad, psyqo::AdvancedPad::Button::Square)) {
            m_sprinting = true;
        }
    } else {
        if (m_input.isButtonPressed(m_pad, psyqo::AdvancedPad::Button::L3)) {
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
        if (m_input.isButtonPressed(m_pad, psyqo::AdvancedPad::Button::L1)) {
            playerPosition.y += dtSpeed;
        }
        if (m_input.isButtonPressed(m_pad, psyqo::AdvancedPad::Button::R1)) {
            playerPosition.y -= dtSpeed;
        }
    }
}

void psxsplash::Controls::sendMotorValues() {
    // Skip SIO transaction when both motors are off - nothing to send.
    if (m_motorSmallCache == 0 && m_motorLargeCache == 0) return;

    using namespace psyqo::Hardware;

    // Send a 0x42 ReadPad command with motor bytes via raw SIO.
    // AdvancedPad's VSync poll sends 0x00 for the motor bytes, so we
    // re-send the desired values here during the game tick.  Motor inertia
    // bridges the brief gap where AdvancedPad zeroes them.
    configurePort(m_port);

    // Every step can now give up rather than spin. This runs EVERY FRAME whenever
    // a motor is on, so it is the hottest raw-SIO0 path in the engine and the one
    // least able to afford an unbounded wait.
    if (!transceive(0x01)) { SIO::Ctrl = 0; return; }  // byte 0: device select
    if (!waitForAck()) { SIO::Ctrl = 0; return; }

    if (!transceive(0x42)) { SIO::Ctrl = 0; return; }  // byte 1: ReadPad command
    if (!waitForAck()) { SIO::Ctrl = 0; return; }

    if (!transceive(0x00)) { SIO::Ctrl = 0; return; }  // byte 2: TAP (ignored)
    if (!waitForAck()) { SIO::Ctrl = 0; return; }

    // byte 3: small motor (right, on/off)
    if (!transceive(m_motorSmallCache)) { SIO::Ctrl = 0; return; }
    if (!waitForAck()) { SIO::Ctrl = 0; return; }

    transceive(m_motorLargeCache);  // byte 4: large motor (left, 0-255)
    // No more bytes needed; remaining response bytes are button/analog data
    // which we don't care about (AdvancedPad already reads them).

    SIO::Ctrl = 0;
}