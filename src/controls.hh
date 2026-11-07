#pragma once

#include <psyqo/advancedpad.hh>
#include <psyqo/trigonometry.hh>
#include <psyqo/vector.hh>
#include <psyqo/fixed-point.hh>

#include "inputmap.hh"

namespace psxsplash {

using namespace psyqo::fixed_point_literals;
using namespace psyqo::trig_literals;

class Controls {
  public:
    /// Force DualShock into analog mode
    /// Must be called BEFORE Init() since Init() hands SIO control to AdvancedPad.
    /// Reaches slot A of each port only: a multitap passes the plain 01h commands
    /// to slot A, and pads in slots B-D stay in whatever mode they are in.
    ///
    /// ONCE PER BOOT, like Init(). The pad latches analog mode itself (that is
    /// what the 0x44/0x03 "lock" byte in the sequence is for), so it survives a
    /// scene change and re-sending it buys nothing. It is not free either: the
    /// whole sequence is four raw SIO0 commands per port, each a chain of
    /// unbounded busy-waits, and running it while the SIO1 receive interrupt is
    /// live is exactly the window in which a pad gives up mid-transfer. See
    /// Init() for the full story.
    static void forceAnalogMode();

    /// Hand SIO0 to psyqo's AdvancedPad. IDEMPOTENT, AND THAT IS LOAD-BEARING.
    ///
    /// There is ONE AdvancedPad for all eight players (s_input), polled in
    /// PollingMode::Fast: both ports every frame (two port reads per frame), and
    /// all four slots of a port with a multitap.
    ///
    /// psyqo::AdvancedPad::initialize() ends with
    ///
    ///     Kernel::Internal::addOnFrame([this]() { readPad(); ... });
    ///
    /// and that list (kernel.cpp: s_beginFrameEvents) is APPEND-ONLY -- there is no
    /// removal API, nothing clears it between scenes, and its fixed_vector has
    /// overflow enabled, so past 32 entries it spills to the heap and keeps
    /// growing. Every registered copy runs every frame, from inside GPU::flip().
    ///
    /// SceneManager::InitializeScene calls this on every scene load, so without
    /// this guard the console permanently gained two more blocking, bit-banged
    /// SIO0 pad polls per frame on every scene transition:
    /// ~0.8ms per frame after the menu, ~2.4ms by the time the game scene is up,
    /// and climbing by ~0.8ms with every lobby->game round after that.
    ///
    /// THAT IS WHAT MADE IT A FREEZE rather than a slowdown. AdvancedPad::readPad
    /// waits for the pad with `while (!(SIO::Stat & STAT_RXRDY));` and no timeout,
    /// and a PlayStation pad ABORTS a transfer if the console does not clock the
    /// next byte promptly after ACK. Under the retail BIOS an SIO1 receive
    /// interrupt costs on the order of 150us to dispatch; one landing between two
    /// pad bytes ends the transfer, and the next wait never returns. More polls
    /// per frame means more windows for that to happen, which is why the freeze
    /// arrived sooner the longer a session ran -- and why it only ever happened
    /// with the link up.
    ///
    /// PCSX-Redux cannot show any of this: it resolves RxMode to Polled and masks
    /// IRQ8 outright (sio1.cpp), its SIO0 answers instantly so the missing timeout
    /// is unreachable, and with no wire the extra polls cost nothing. "It works on
    /// the emulator" was the absence of evidence here.
    ///
    /// The callback lives on the Kernel, not on the scene, so registering once per
    /// boot is not merely cheaper -- it is the correct lifetime.
    ///
    /// Init also registers, right after the poll, the callback that adds each
    /// mouse report to its slot's motion sum (see mouseDelta).
    static void Init();

    /// Which player (1..8) this instance reads; see inputmap::slotForPlayer.
    /// Player 1 is port 1 (Pad1a), player 2 port 2 (Pad2a), players 3-5 port 1's
    /// multitap slots B-D and players 6-8 port 2's.
    void setPlayer(int player);
    int player() const { return m_player; }

    /// Movement/look handling for this instance's player. A mouse moves nothing.
    void HandleControls(psyqo::Vec3 &playerPosition, psyqo::Angle &playerRotationX, psyqo::Angle &playerRotationY,
                        psyqo::Angle &playerRotationZ, bool freecam, int32_t dt12);

    /// Update button state tracking, the sticks and the mouse motion - call once
    /// per frame, before HandleControls.
    void UpdateButtonStates();

    /// What is plugged into this player's slot, as of the last poll.
    inputmap::Device device() const;

    /// Mouse motion over the last frame: every report polled between the last
    /// two UpdateButtonStates calls, summed. 0, 0 unless the device is a mouse.
    int32_t mouseDeltaX() const { return m_mouseDX; }
    int32_t mouseDeltaY() const { return m_mouseDY; }
    
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
    
    /// Check if a button is currently held (the latest poll, not this frame's latch)
    bool isButtonHeld(psyqo::AdvancedPad::Button button) const {
        return (liveButtons() >> static_cast<uint16_t>(button)) & 1;
    }
    
    /// Get bitmask of buttons pressed this frame
    uint16_t getButtonsPressed() const { return m_buttonsPressed; }
    
    /// Get bitmask of buttons released this frame
    uint16_t getButtonsReleased() const { return m_buttonsReleased; }

    /// Analog stick accessors (set during UpdateButtonStates). The D-pad stands in
    /// for the left stick on a pad without sticks; a mouse reads 0.
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

    /// Bracket psyqo's per-frame pad poll, which is the one place in the whole
    /// program that can destroy the SIO1 receive interrupt. Call Begin BEFORE the
    /// first Init() and End AFTER the last one; both are once-per-boot no-ops
    /// afterwards.
    ///
    /// WHY THE PAD POLL, of all things, is a network hazard: psyqo's
    /// AdvancedPad::transceive() clears the CONTROLLER interrupt before every pad
    /// byte, and psyqo implements that as a read-modify-write on I_STAT. I_STAT is
    /// acknowledge-by-writing-zero, so it writes back a zero for every bit that was
    /// not pending when it read - including ours, if SIO1 asserted in between. And
    /// because SIO_STAT.9 is sticky while I_STAT.8 is edge-triggered, that
    /// acknowledgement is FINAL: no further edge is ever generated and the receive
    /// interrupt is dead for the session. Full mechanism on Sio1::rearmRx().
    ///
    /// THE FIRST VERSION OF THIS MASKED IRQ8 ACROSS THE POLL, AND THAT WAS WORSE
    /// THAN NOTHING. I_MASK gates DISPATCH; it does not stop I_STAT.8 being set. So
    /// masking simply left our pending bit latched for the whole poll, maximally
    /// exposed to the very read-modify-writes that clear it. It is recorded here
    /// because it looks like an obvious fix and is the opposite of one.
    ///
    /// What actually works is to drain the FIFO going in and re-arm the interrupt
    /// coming out, so a destroyed edge is recreated before it can cost a byte. The
    /// arithmetic is what makes this exact rather than hopeful: a pad poll is ~9
    /// bytes at SIO0's 250kHz plus setup, about 380us, twice (one per player) -
    /// ~760us. At 57600 that is 4.4 bytes arriving into an 8-byte FIFO that Begin
    /// just emptied. **The FIFO cannot overflow inside the window**, so re-arming
    /// immediately after it means no byte is ever lost, even when the race fires.
    ///
    /// A MULTITAP BREAKS THAT ARITHMETIC. psyqo's read of a tapped port is 35
    /// bytes, not 9: ten at 250kHz (slot A and the header) and 25 at 1MHz (slots
    /// B-D), roughly twice a plain pad's time before ACK delays. One tap still
    /// fits the FIFO; two taps put the window near 8 bytes at 57600, and a byte
    /// can be lost when the race fires. Not measured on hardware.
    ///
    /// Both halves are no-ops in RxMode::Polled, which is what PCSX-Redux resolves
    /// to: there is no interrupt there to lose, and Sio1::rearmRx() returns
    /// immediately. The emulator's behaviour is unchanged by any of this.
    static void ShieldPadPollBegin();
    static void ShieldPadPollEnd();

    /// How many times a Controls instance has actually handed SIO0 to AdvancedPad
    /// since boot -- i.e. how many entries this class has added to the kernel's
    /// per-frame callback list, which nothing can ever remove.
    ///
    /// A DIAGNOSTIC WITH A FIXED EXPECTED VALUE, which is what makes it useful on
    /// a console with no other channel: it must read 1 (the one shared
    /// AdvancedPad) and stay there for the life of the boot. Anything higher means
    /// the guard in Init() has been defeated and the per-frame pad-poll cost is
    /// growing again. It read 2 x (scene loads) before that guard existed.
    static uint32_t padInitCount() { return s_padInitCount; }

  private:
    // The one pad driver for all players, and the one sine table for all of them
    // (eight instances of a 2KB table would be 16KB for nothing).
    static psyqo::AdvancedPad s_input;
    static psyqo::Trig<> s_trig;

    // Each slot's mouse motion since its player's last UpdateButtonStates, added
    // to by the per-poll callback Init registers.
    static inputmap::MouseMotion s_mouseMotion[inputmap::MAX_PLAYERS];

    // See Init() / forceAnalogMode(). Both are once per boot.
    static bool s_padInitialized;
    static bool s_analogForced;
    static uint32_t s_padInitCount;

    // See ShieldPadPollBegin/End. The registrations are once-per-boot for the same
    // reason Init() is: addOnFrame has no removal API.
    static bool s_shieldBeginInstalled;
    static bool s_shieldEndInstalled;

    // Which physical controller this instance reads. Set by setPlayer; defaults
    // to player 1 (controller in port 1).
    int m_player = 1;
    psyqo::AdvancedPad::Pad m_pad = psyqo::AdvancedPad::Pad::Pad1a;
    uint8_t m_port = 0;

    /// The buttons held at the latest poll, active-high (inputmap::heldMask).
    uint16_t liveButtons() const;
    bool held(psyqo::AdvancedPad::Button button) const { return (liveButtons() >> button) & 1; }

    /// Stick values from the latest poll, into m_leftStickX..m_rightStickY.
    void updateSticks(inputmap::Device d);

    bool m_sprinting = false;
    static constexpr uint8_t m_stickDeadzone = 0x30;
    static constexpr psyqo::Angle rotSpeed = 0.02_pi;
    
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

    // Mouse motion latched by UpdateButtonStates
    int32_t m_mouseDX = 0;
    int32_t m_mouseDY = 0;

    /// Get movement axes from D-pad as simulated stick values (-127 to +127)
    void getDpadAxes(int16_t &outX, int16_t &outY) const;

    /// Send cached motor values to the controller via raw SIO.
    /// Called each frame from UpdateButtonStates() after AdvancedPad's VSync poll.
    /// Reaches the pad in slot A of this player's port, through a multitap too
    /// (inputmap::motorCommand); slots B-D get no rumble.
    void sendMotorValues();
};

}  // namespace psxsplash