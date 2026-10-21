#pragma once

#include <stdint.h>

// The player's vertical motion (jump, gravity, falling), free of psyqo so that
// tests/host/test_jump.cpp can compile this exact code natively.
//
// Units are the splashpack's: positions are 20.12 world units (4096 = one GTE
// unit), the jump velocity is per SECOND and gravity per second squared, as
// both SplashEdit exporters write them (sqrt(2 g h) / gte * 4096 and
// g / gte * 4096). Time comes in as dt12, where 4096 is one 30 fps frame.
//
// Integration is trapezoidal (the position moves by the mean of the old and
// new velocity), which is exact under constant gravity, and both integrals
// carry their remainders, so a jump peaks at the exported height whatever
// the frame rate and however small the per-frame steps are at a large
// GTE scaling.

namespace psxsplash {
namespace jumpmath {

/// dt12 units in one second (dt12 4096 = one 1/30 s frame).
static constexpr int32_t DT12_PER_SECOND = 4096 * 30;

/// Terminal velocity, as twice the seconds of free fall that reach it: 2.5 s,
/// which at 20 m/s^2 is 50 m/s whatever the GTE scaling (the engine never
/// learns the scaling, so it cannot be a fixed number of metres per second).
static constexpr int32_t TERMINAL_FALL_SECONDS_X2 = 5;

struct VerticalMotion {
    int32_t velocity = 0;  ///< 20.12 units per second, PSX Y-down (negative is up)
    int32_t velRem = 0;    ///< velocity carry, in 1/DT12_PER_SECOND
    int32_t posRem = 0;    ///< position carry, in 1/(2 * DT12_PER_SECOND), within [-1/2, 1/2) unit
};

/// Fastest fall: 2.5 s worth of gravity, but never below the jump's
/// own launch speed, so a jump back to the same floor falls as long as it rose.
inline int32_t terminalVelocity(int32_t gravityPerSec2, int32_t jumpVelocityPerSec) {
    int32_t t = gravityPerSec2 * TERMINAL_FALL_SECONDS_X2 / 2;
    return t > jumpVelocityPerSec ? t : jumpVelocityPerSec;
}

inline void stop(VerticalMotion& m) {
    m.velocity = 0;
    m.velRem = 0;
    m.posRem = 0;
}

inline void launch(VerticalMotion& m, int32_t jumpVelocityPerSec) {
    m.velocity = -jumpVelocityPerSec;
    m.velRem = 0;
    m.posRem = 0;
}

/// Advances by dt12 under gravity, capped at the terminal velocity, and
/// returns the Y delta (20.12) to add to the position.
///
/// All 32-bit (the PS1 build has no 64-bit divide): gravity is a u16, dt12 is
/// at most 4 frames (16384), so g * dt12 < 2^30, and every velocity stays
/// within the u16 jump velocity or 2.5 * the u16 gravity.
inline int32_t step(VerticalMotion& m, int32_t gravityPerSec2, int32_t terminal, int32_t dt12) {
    int32_t dv = gravityPerSec2 * dt12 + m.velRem;
    int32_t dvi = dv / DT12_PER_SECOND;
    m.velRem = dv - dvi * DT12_PER_SECOND;

    int32_t v0 = m.velocity;
    int32_t v1 = v0 + dvi;
    if (v1 > terminal) {
        v1 = terminal;
        m.velRem = 0;
    }
    m.velocity = v1;

    // dy = (v0 + v1) * dt12 / (2 * DT12_PER_SECOND), i.e. sum * (hi * 4096 + lo) / (60 * 4096):
    // the whole frames of dt12 are divided by 60 first so nothing overflows.
    int32_t sum = v0 + v1;
    int32_t hi = sum * (dt12 >> 12);
    int32_t q = hi / 60;
    // The carry is kept in [-1/2, 1/2) of a unit so Y is the exact position rounded to
    // the nearest unit (truncating would sit up to a whole unit low all the way up).
    const int32_t den = 2 * DT12_PER_SECOND;
    int32_t num = (hi - q * 60) * 4096 + sum * (dt12 & 4095) + m.posRem + den / 2;
    int32_t dyi = num / den;
    if (num - dyi * den < 0) dyi--;  // floor
    m.posRem = num - dyi * den - den / 2;
    return q + dyi;
}

}  // namespace jumpmath
}  // namespace psxsplash
