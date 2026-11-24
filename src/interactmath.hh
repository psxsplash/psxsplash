#pragma once

// Interaction range math: the check in SceneManager::updateInteractionSystem
// and the conversions behind Interact.SetRadius / GetRadius. psyqo-free so the
// host tests compile it natively.
//
// The interactable keeps its radius squared, in fp12 world units like every
// other position (Interactable::radiusSquared, one raw step = 1/4096 of a
// square unit). Scene distances are small fractions of a unit (at gteScaling
// 100 a 2 m reach is 0.02 units, 82 raw), so the squared distance is summed
// exactly from the raw values in 64 bits and only then floored to fp12. The
// object is in range while that floor is at most radiusSquared, which makes
// the reach the largest distance whose square floors to it: never shorter than
// the radius it was set from, longer by less than one step of the square.

#include <stdint.h>

namespace psxsplash {
namespace interactmath {

// Largest radius (raw fp12) whose square still fits an int32 after the floor:
// floor(sqrt(2^43)), about 724 units.
static constexpr int32_t kMaxRadiusRaw = 2965820;

// Exact squared distance, raw fp12 squared (2^24 per square unit).
inline uint64_t distanceSquared(int32_t dx, int32_t dy, int32_t dz) {
    return (uint64_t)((int64_t)dx * dx) + (uint64_t)((int64_t)dy * dy) + (uint64_t)((int64_t)dz * dz);
}

inline bool inRange(uint64_t distSq, int32_t radiusSquared) {
    if (radiusSquared < 0) return false;
    return (distSq >> 12) <= (uint64_t)radiusSquared;
}

// Radius (raw fp12) -> squared radius (raw fp12). Negative radii count as 0,
// radii past kMaxRadiusRaw are clamped.
inline int32_t radiusSquaredFromRadius(int32_t radius) {
    if (radius <= 0) return 0;
    if (radius > kMaxRadiusRaw) radius = kMaxRadiusRaw;
    return (int32_t)(((int64_t)radius * radius) >> 12);
}

// Floor of the square root of a 64-bit value, by bits (no divide, so no 64-bit
// divide helper on the R3000).
inline uint32_t isqrt64(uint64_t v) {
    uint64_t r = 0;
    for (uint64_t bit = (uint64_t)1 << 62; bit; bit >>= 2) {
        if (v >= r + bit) {
            v -= r + bit;
            r = (r >> 1) + bit;
        } else {
            r >>= 1;
        }
    }
    return (uint32_t)r;
}

// Squared radius (raw fp12) -> the reach it gives (raw fp12): the largest
// distance inRange still accepts. Setting it again gives the same square back.
inline int32_t radiusFromRadiusSquared(int32_t radiusSquared) {
    if (radiusSquared < 0) return 0;
    return (int32_t)isqrt64(((uint64_t)radiusSquared << 12) + 4095);
}

}  // namespace interactmath
}  // namespace psxsplash
