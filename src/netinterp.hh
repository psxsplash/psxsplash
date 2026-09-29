#pragma once

#include <stdint.h>

/**
 * Remote-avatar interpolation math.
 *
 * Pure arithmetic, no engine types, so it can be compiled and tested on a host -
 * the same reason spritemath and tilemath exist. This is gameplay-visible
 * behaviour that cannot be checked on a PlayStation without burning a disc, which
 * makes it exactly the sort of thing worth being able to test without one.
 *
 * THE MODEL. A remote avatar is drawn walking from where it was to where the
 * newest snapshot says it is, across the interval between those two snapshots.
 * It is NOT eased toward the newest position.
 *
 * That distinction is the whole point. Exponential easing converges on its target
 * and then stops, so between packets the avatar has nothing to do: at the 15Hz a
 * serial console receives, it moved for a few frames and stood still for the rest
 * of each 67ms window. Measured on real hardware as "other players' movement is
 * choppy" while the local player looked perfect - and that asymmetry was the clue,
 * because the local avatar is driven by input every frame and never runs out of
 * work to do.
 *
 * Interpolating between two known points costs one snapshot interval of latency.
 * That is the trade every networked game makes, and it is invisible next to the
 * stutter it removes.
 */
namespace psxsplash::net {

/// Progress through an interpolation leg, in 4.12 fixed point (4096 == arrived).
///
/// CLAMPED AT 1, deliberately: when a snapshot is late the avatar waits at the
/// last known position rather than extrapolating past it. Guessing forward is what
/// slides a player through a wall on a hiccup, and this link hiccups - a console
/// has been measured taking FIFO overruns mid-game. A brief pause is a far cheaper
/// artefact than a body in the geometry.
constexpr int32_t lerpAlpha(int32_t elapsedDt, int32_t intervalDt) {
    if (intervalDt <= 0) return 4096;
    if (elapsedDt <= 0) return 0;
    if (elapsedDt >= intervalDt) return 4096;
    return (elapsedDt * 4096) / intervalDt;
}

/// One component of the walk from `prev` to `cur` at `alpha`.
///
/// Multiplies THEN divides so truncation rounds toward zero: a negative delta
/// rounding away from the target would leave an avatar creeping and never
/// settling. Callers must bound |cur - prev| (see c_lerpSnapDistance) so the
/// intermediate stays inside int32.
constexpr int32_t lerpComponent(int32_t prev, int32_t cur, int32_t alpha) {
    return prev + ((cur - prev) * alpha) / 4096;
}

}  // namespace psxsplash::net
