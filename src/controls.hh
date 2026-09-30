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
    ///
    /// ONCE PER BOOT, like Init(). The pad latches analog mode itself (that is
    /// what the 0x44/0x03 "lock" byte in the sequence is for), so it survives a
    /// scene change and re-sending it buys nothing. It is not free either: the
    /// whole sequence is four raw SIO0 commands per port, each a chain of
    /// unbounded busy-waits, and running it while the SIO1 receive interrupt is
    /// live is exactly the window in which a pad gives up mid-transfer. See
    /// Init() for the full story.
    void forceAnalogMode();

    /// Hand SIO0 to psyqo's AdvancedPad. IDEMPOTENT, AND THAT IS LOAD-BEARING.
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
    /// SceneManager::InitializeScene calls this for BOTH players on every scene
    /// load, so without this guard the console permanently gained two more
    /// blocking, bit-banged SIO0 pad polls per frame on every scene transition:
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
    void Init();

    /// Per-player movement/look handling.
    /// Player 1 reads the controller in port 1 (Pad1a); player 2 reads port 2 (Pad2a).
    void HandleControlsPlayer1(psyqo::Vec3 &playerPosition, psyqo::Angle &playerRotationX, psyqo::Angle &playerRotationY,
                               psyqo::Angle &playerRotationZ, bool freecam, int32_t dt12);
    void HandleControlsPlayer2(psyqo::Vec3 &playerPosition, psyqo::Angle &playerRotationX, psyqo::Angle &playerRotationY,
                               psyqo::Angle &playerRotationZ, bool freecam, int32_t dt12);

    /// Update button state tracking - call before HandleControls* each frame. One per player.
    void UpdateButtonStatesPlayer1();
    void UpdateButtonStatesPlayer2();
    
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
        return m_input.isButtonPressed(m_pad, button);
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

    /// How many times a Controls instance has actually handed SIO0 to AdvancedPad
    /// since boot -- i.e. how many entries this class has added to the kernel's
    /// per-frame callback list, which nothing can ever remove.
    ///
    /// A DIAGNOSTIC WITH A FIXED EXPECTED VALUE, which is what makes it useful on
    /// a console with no other channel: it must read 2 (one per player) and stay
    /// there for the life of the boot. Anything higher means the guard in Init()
    /// has been defeated and the per-frame pad-poll cost is growing again. It read
    /// 2 x (scene loads) before that guard existed.
    static uint32_t padInitCount() { return s_padInitCount; }

  private:
    psyqo::AdvancedPad m_input;
    psyqo::Trig<> m_trig;

    // See Init() / forceAnalogMode(). Both are once-per-boot, per instance.
    bool m_padInitialized = false;
    bool m_analogForced = false;
    static uint32_t s_padInitCount;

    // Which physical controller this instance drives. Set by the PlayerN entry
    // points; defaults to player 1 (controller in port 1).
    psyqo::AdvancedPad::Pad m_pad = psyqo::AdvancedPad::Pad::Pad1a;
    uint8_t m_port = 0;

    // Shared cores, operating on m_pad / m_port. The public PlayerN methods set
    // the identity and then delegate here.
    void updateButtonStates();
    void handleControls(psyqo::Vec3 &playerPosition, psyqo::Angle &playerRotationX, psyqo::Angle &playerRotationY,
                        psyqo::Angle &playerRotationZ, bool freecam, int32_t dt12);

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
    
    /// Returns true if the connected pad is digital-only (no analog sticks)
    bool isDigitalPad() const;
    
    /// Get movement axes from D-pad as simulated stick values (-127 to +127)
    void getDpadAxes(int16_t &outX, int16_t &outY) const;

    /// Send cached motor values to the controller via raw SIO.
    /// Called each frame from UpdateButtonStates() after AdvancedPad's VSync poll.
    void sendMotorValues();
};

}  // namespace psxsplash