#include "controls.hh"

#include <psyqo/hardware/cpu.hh>
#include <psyqo/hardware/sio.hh>
#include <psyqo/kernel.hh>
#include <psyqo/sio0-bus.hh>
#include <psyqo/vector.hh>

#include "irqack.hh"
#include "features.hh"
#include "sio1.hh"

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

psyqo::AdvancedPad psxsplash::Controls::s_input;
psyqo::Trig<> psxsplash::Controls::s_trig;
psxsplash::inputmap::MouseMotion psxsplash::Controls::s_mouseMotion[psxsplash::inputmap::MAX_PLAYERS];
bool psxsplash::Controls::s_padInitialized = false;
bool psxsplash::Controls::s_analogForced = false;
uint32_t psxsplash::Controls::s_padInitCount = 0;
bool psxsplash::Controls::s_shieldBeginInstalled = false;
bool psxsplash::Controls::s_shieldEndInstalled = false;

// The two halves of the pad-poll bracket. Registration order is execution order
// (Kernel::Internal::beginFrame walks its list front to back), so these must be
// registered on either side of AdvancedPad::initialize() -- which is what
// SceneManager::InitializeScene does. The full reasoning is on the declarations in
// controls.hh; it lives there because that is where somebody removing this will be
// reading.
void psxsplash::Controls::ShieldPadPollBegin() {
    if (s_shieldBeginInstalled) return;
    s_shieldBeginInstalled = true;
#if PSXSPLASH_FEATURE_NET
    psyqo::Kernel::Internal::addOnFrame([]() {
        // Go in with an EMPTY FIFO and a live interrupt. The pad poll is ~760us,
        // and 760us at 57600 is 4.4 bytes -- so an empty FIFO cannot overflow
        // inside the window, whereas a half-full one can. That is what turns this
        // from damage limitation into "no byte is ever lost".
        //
        // rearmRx(), NOT poll(): poll() also runs pumpTxBlocking, whose 48-byte
        // budget can spend 8.3ms. Calling it from the frame callback would add a
        // second full transmit pass per frame from inside GPU::flip.
        Sio1::Get().rearmRx();
    });
#endif
}

void psxsplash::Controls::ShieldPadPollEnd() {
    if (s_shieldEndInstalled) return;
    s_shieldEndInstalled = true;
#if PSXSPLASH_FEATURE_NET
    psyqo::Kernel::Internal::addOnFrame([]() {
        // Recreate the interrupt's edge, immediately after the only code in the
        // program that can have destroyed it. This is the tight net; Sio1::poll()
        // carries a second one for everything else. See Sio1::rearmRx().
        Sio1::Get().rearmRx();
    });
#endif
}

void psxsplash::Controls::forceAnalogMode() {
    // Once per boot. The pad latches analog mode itself, so a scene change does
    // not undo it -- and this is four raw SIO0 command chains per port, every byte
    // of which is a busy-wait. See the header.
    if (s_analogForced) return;
    s_analogForced = true;

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
    if (s_padInitialized) return;
    s_padInitialized = true;
    s_padInitCount++;
    // Fast: both ports every frame (Normal reads one port per frame). With a
    // multitap that is all four of its slots too.
    s_input.initialize(psyqo::AdvancedPad::PollingMode::Fast);

    // Registered right after the poll, so it runs after every one of them: one
    // mouse report per poll per slot, summed until the player's next
    // UpdateButtonStates takes it. No SIO here, a few dozen loads.
    psyqo::Kernel::Internal::addOnFrame([]() {
        // readPad stood down for a memory card transaction: what it holds is the
        // previous poll's report, already counted.
        if (psyqo::SIO0Bus::owned()) return;
        for (unsigned slot = 0; slot < inputmap::MAX_PLAYERS; slot++) {
            auto pad = static_cast<psyqo::AdvancedPad::Pad>(slot);
            if (inputmap::classify(s_input.isPadConnected(pad), s_input.getPadType(pad)) != inputmap::Device::Mouse)
                continue;
            s_mouseMotion[slot].addReport(s_input.getAdc(pad, 0), s_input.getAdc(pad, 1));
        }
    });
}

void psxsplash::Controls::setPlayer(int player) {
    int slot = inputmap::slotForPlayer(player);
    if (slot < 0) return;
    m_player = player;
    m_pad = static_cast<psyqo::AdvancedPad::Pad>(slot);
    m_port = static_cast<uint8_t>(inputmap::portForPlayer(player));
}

psxsplash::inputmap::Device psxsplash::Controls::device() const {
    return inputmap::classify(s_input.isPadConnected(m_pad), s_input.getPadType(m_pad));
}

uint16_t psxsplash::Controls::liveButtons() const {
    return inputmap::heldMask(device(), s_input.getHalfword(m_pad, 1));
}

void psxsplash::Controls::getDpadAxes(int16_t &outX, int16_t &outY) const {
    outX = 0;
    outY = 0;
    // D-pad produces full-magnitude values (like pushing the stick to the edge)
    if (held(psyqo::AdvancedPad::Button::Up))
        outY = -127;
    if (held(psyqo::AdvancedPad::Button::Down))
        outY = 127;
    if (held(psyqo::AdvancedPad::Button::Left))
        outX = -127;
    if (held(psyqo::AdvancedPad::Button::Right))
        outX = 127;
}

void psxsplash::Controls::UpdateButtonStates() {
    m_previousButtons = m_currentButtons;

    // Send motor values via raw SIO (after AdvancedPad's VSync poll has completed)
    sendMotorValues();

    // Every button as one active-high mask. A mouse keeps only its two buttons;
    // its always-zero bits would otherwise hold L2 and R2 down.
    inputmap::Device d = device();
    m_currentButtons = inputmap::heldMask(d, s_input.getHalfword(m_pad, 1));

    // Calculate pressed and released buttons
    m_buttonsPressed = m_currentButtons & ~m_previousButtons;
    m_buttonsReleased = m_previousButtons & ~m_currentButtons;

    updateSticks(d);

    unsigned slot = static_cast<unsigned>(m_pad);
    s_mouseMotion[slot].latch(m_mouseDX, m_mouseDY);
    if (d != inputmap::Device::Mouse) m_mouseDX = m_mouseDY = 0;
}

void psxsplash::Controls::updateSticks(inputmap::Device d) {
    int16_t rightXOffset = 0, rightYOffset = 0, leftXOffset = 0, leftYOffset = 0;

    if (d == inputmap::Device::Mouse) {
        // A mouse's motion sits where the right stick would be; it is not a stick.
    } else if (!inputmap::hasSticks(d)) {
        // Digital pad (or nothing, a gun, a NegCon...): D-pad for movement, L1/R1
        // for rotation (no vertical look on digital).
        getDpadAxes(leftXOffset, leftYOffset);
        if (held(psyqo::AdvancedPad::Button::R1))
            rightXOffset = 90;
        if (held(psyqo::AdvancedPad::Button::L1))
            rightXOffset = -90;
    } else {
        // Analog pad or stick: read stick ADC values
        uint8_t rightX = s_input.getAdc(m_pad, 0);
        uint8_t rightY = s_input.getAdc(m_pad, 1);
        uint8_t leftX = s_input.getAdc(m_pad, 2);
        uint8_t leftY = s_input.getAdc(m_pad, 3);

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

    m_leftStickX = leftXOffset;
    m_leftStickY = leftYOffset;
    m_rightStickX = rightXOffset;
    m_rightStickY = rightYOffset;
}

void psxsplash::Controls::HandleControls(psyqo::Vec3 &playerPosition, psyqo::Angle &playerRotationX,
                                         psyqo::Angle &playerRotationY, psyqo::Angle &playerRotationZ, bool freecam,
                                         int32_t dt12) {
    inputmap::Device d = device();
    // A mouse moves nothing: its buttons are where L1/R1 are, which would turn.
    if (d == inputmap::Device::Mouse) {
        m_sprinting = false;
        return;
    }
    bool digital = !inputmap::hasSticks(d);

    // Sticks (or the D-pad standing in for them) as UpdateButtonStates read them.
    int16_t leftXOffset = m_leftStickX, leftYOffset = m_leftStickY;
    int16_t rightXOffset = m_rightStickX, rightYOffset = m_rightStickY;

    // Sprint toggle (L3 for analog, Square for digital)
    if (__builtin_abs(leftXOffset) < m_stickDeadzone && __builtin_abs(leftYOffset) < m_stickDeadzone) {
        m_sprinting = false;
    }

    if (digital) {
        if (held(psyqo::AdvancedPad::Button::Square)) {
            m_sprinting = true;
        }
    } else {
        if (held(psyqo::AdvancedPad::Button::L3)) {
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
        playerPosition.x += s_trig.sin(playerRotationY) * forward;
        playerPosition.z += s_trig.cos(playerRotationY) * forward;
    }
    if (__builtin_abs(leftXOffset) > m_stickDeadzone) {
        psyqo::FixedPoint<12> strafe = -(leftXOffset * speed) >> 7;
        strafe.value = (int32_t)(((int64_t)strafe.value * dt12) >> 12);
        playerPosition.x -= s_trig.cos(playerRotationY) * strafe;
        playerPosition.z += s_trig.sin(playerRotationY) * strafe;
    }

    if (freecam) {
        psyqo::FixedPoint<12> dtSpeed;
        dtSpeed.value = (int32_t)(((int64_t)speed.value * dt12) >> 12);
        if (held(psyqo::AdvancedPad::Button::L1)) {
            playerPosition.y += dtSpeed;
        }
        if (held(psyqo::AdvancedPad::Button::R1)) {
            playerPosition.y -= dtSpeed;
        }
    }
}

void psxsplash::Controls::sendMotorValues() {
    // Skip SIO transaction when both motors are off - nothing to send.
    if (m_motorSmallCache == 0 && m_motorLargeCache == 0) return;

    // Slot A only (see motorCommand), and only when it holds something: talking
    // to an empty slot A through a tap would cancel its long read for nothing.
    unsigned slot = static_cast<unsigned>(m_pad);
    if (slot % 4 != 0 || !s_input.isPadConnected(m_pad)) return;

    using namespace psyqo::Hardware;

    // A 0x42 ReadPad command with motor bytes via raw SIO.
    // AdvancedPad's VSync poll sends 0x00 for the motor bytes, so we
    // re-send the desired values here during the game tick.  Motor inertia
    // bridges the brief gap where AdvancedPad zeroes them.
    auto status = [](psyqo::AdvancedPad::Pad pad) -> uint8_t { return s_input.getHalfword(pad, 0) & 0xff; };
    bool tap = inputmap::tapPresent(true, status(static_cast<psyqo::AdvancedPad::Pad>(slot + 1)),
                                    status(static_cast<psyqo::AdvancedPad::Pad>(slot + 2)),
                                    status(static_cast<psyqo::AdvancedPad::Pad>(slot + 3)));
    uint8_t cmd[7];
    unsigned len = inputmap::motorCommand(tap, m_motorSmallCache, m_motorLargeCache, cmd);

    configurePort(m_port);

    // Every step can give up rather than spin. This runs EVERY FRAME whenever a
    // motor is on, so it is the hottest raw-SIO0 path in the engine and the one
    // least able to afford an unbounded wait. The pad's remaining reply bytes
    // (buttons, sticks) are not clocked out; AdvancedPad already read them.
    sendCommand(cmd, len);

    SIO::Ctrl = 0;
}
