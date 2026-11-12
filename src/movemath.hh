#pragma once

#include <stdint.h>

// Stick-driven walking and turning, free of psyqo so that tests/host/test_move.cpp
// can compile this exact code natively.
//
// A stick offset is in [-128, 127] (zero inside the deadzone), the speed is the
// pack's per-frame fp12 move speed, dt12 is 4096 per 30 fps frame and sin/cos
// are fp12. The whole step is one exact product, and the part below one unit is
// carried to the next tick. Shifting each factor down separately floors every
// partial result toward minus infinity, which makes the negative directions
// (backward, right strafe) faster than the positive ones: with a 4-unit move
// speed, left strafe moved 3 units a tick and right strafe 6.

namespace psxsplash {
namespace movemath {

/// The product's scale below one unit: 7 bits of stick, 12 of dt12, 12 of trig.
static constexpr int MOVE_SHIFT = 7 + 12 + 12;
/// 7 bits of stick and 12 of dt12.
static constexpr int TURN_SHIFT = 7 + 12;

struct Carry {
    int64_t x = 0;  ///< remainders, in 1 / 2^MOVE_SHIFT unit, within [0, 1) unit
    int64_t z = 0;
};

/// Floors num / 2^shift and leaves the remainder in num.
inline int32_t takeWhole(int64_t& num, int shift) {
    int64_t whole = num >> shift;
    num -= whole << shift;
    return static_cast<int32_t>(whole);
}

/// Moves by the left stick: stickY < 0 walks forward, stickX > 0 strafes right.
/// Facing yaw has sin/cos sinY, cosY; forward is (+sin, +cos) in XZ and right
/// is (+cos, -sin).
inline void walk(int32_t stickX, int32_t stickY, int32_t speed, int32_t dt12, int32_t sinY, int32_t cosY, Carry& c,
                 int32_t& dx, int32_t& dz) {
    int64_t fwd = -(int64_t)stickY * speed * dt12;
    int64_t right = (int64_t)stickX * speed * dt12;
    c.x += (int64_t)sinY * fwd + (int64_t)cosY * right;
    c.z += (int64_t)cosY * fwd - (int64_t)sinY * right;
    dx = takeWhole(c.x, MOVE_SHIFT);
    dz = takeWhole(c.z, MOVE_SHIFT);
}

/// Turns by a stick axis at turnSpeed angle units per frame at full deflection;
/// carry is in 1 / 2^TURN_SHIFT angle unit.
inline int32_t turn(int32_t stick, int32_t turnSpeed, int32_t dt12, int64_t& carry) {
    carry += (int64_t)stick * turnSpeed * dt12;
    return takeWhole(carry, TURN_SHIFT);
}

}  // namespace movemath
}  // namespace psxsplash
