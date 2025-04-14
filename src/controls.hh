#pragma once

#include <psyqo/advancedpad.hh>
#include <psyqo/trigonometry.hh>
#include <psyqo/vector.hh>

namespace psxsplash {

using namespace psyqo::fixed_point_literals;
using namespace psyqo::trig_literals;

class Controls {
  public:
    void Init();
    void HandleControls(psyqo::Vec3 &playerPosition, psyqo::Angle &playerRotationX, psyqo::Angle &playerRotationY,
                        psyqo::Angle &playerRotationZ, bool freecam, int deltaTime);

  private:
    psyqo::AdvancedPad m_input;
    psyqo::Trig<> m_trig;

    bool m_sprinting = false;
    static constexpr uint8_t m_stickDeadzone = 0x30;
    static constexpr psyqo::FixedPoint<12> moveSpeed = 0.002_fp;
    static constexpr psyqo::Angle rotSpeed = 0.01_pi;
    static constexpr psyqo::FixedPoint<12> sprintSpeed = 0.01_fp;
};

};  // namespace psxsplash