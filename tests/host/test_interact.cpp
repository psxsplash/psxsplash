// Host tests for the interaction range math (src/interactmath.hh), compiled
// natively: the distance check, what Interact.SetRadius stores and what
// Interact.GetRadius reads back.

#include <math.h>
#include <stdint.h>

#include "interactmath.hh"

#include "testing.hh"

using namespace psxsplash::interactmath;

// At gteScaling 100 a 2 m reach is 82 raw. The old per-term fp12 squares made
// 71^2 and 41^2 (the two legs of a 2 m distance) 1 and 0, so a 1 m radius took
// it; the exact sum does not.
TEST(exact_distance_small_scale) {
    uint64_t twoMetres = distanceSquared(71, 0, 41);
    CHECK_EQ(twoMetres, 71 * 71 + 41 * 41);
    CHECK(!inRange(twoMetres, radiusSquaredFromRadius(41)));   // 1 m
    CHECK(inRange(twoMetres, radiusSquaredFromRadius(82)));    // 2 m
    CHECK(inRange(twoMetres, radiusSquaredFromRadius(123)));   // 3 m
    CHECK(inRange(0, 0));
    CHECK(!inRange(0, -1));
}

TEST(distance_signs_and_large_values) {
    CHECK_EQ(distanceSquared(-3, 4, 0), 25);
    CHECK_EQ(distanceSquared(-0x7FFFFFFF, 0, 0), (uint64_t)0x7FFFFFFF * 0x7FFFFFFF);
    // One unit away on each axis: sqrt(3) units, square 3 * 4096 raw.
    uint64_t d = distanceSquared(4096, -4096, 4096);
    CHECK(inRange(d, 3 * 4096));
    CHECK(!inRange(d, 3 * 4096 - 1));
}

// A radius set reaches at least that far, and less than one step of the square
// further.
TEST(reach_covers_radius) {
    for (int32_t r = 0; r < 2000000; r = r * 9 / 8 + 1) {
        int32_t sq = radiusSquaredFromRadius(r);
        CHECK(inRange(distanceSquared(r, 0, 0), sq));
        int32_t reach = radiusFromRadiusSquared(sq);
        CHECK(reach >= r);
        CHECK(inRange(distanceSquared(reach, 0, 0), sq));
        CHECK(!inRange(distanceSquared(reach + 1, 0, 0), sq));
        CHECK((double)reach * reach < ((double)sq + 1) * 4096.0);
    }
}

TEST(small_negative_and_large_radii) {
    CHECK_EQ(radiusSquaredFromRadius(0), 0);
    CHECK_EQ(radiusSquaredFromRadius(-4096), 0);
    CHECK_EQ(radiusSquaredFromRadius(41), 0);
    CHECK_EQ(radiusSquaredFromRadius(64), 1);
    CHECK_EQ(radiusSquaredFromRadius(4096), 4096);
    CHECK_EQ(radiusSquaredFromRadius(2048), 1024);
    CHECK_EQ(radiusFromRadiusSquared(0), 63);    // 0 still reaches 1/64 of a unit
    CHECK_EQ(radiusFromRadiusSquared(-5), 0);
    CHECK_EQ(radiusFromRadiusSquared(4096), 4096);
    CHECK(radiusSquaredFromRadius(kMaxRadiusRaw) > 0);
    CHECK_EQ(radiusSquaredFromRadius(kMaxRadiusRaw + 1), radiusSquaredFromRadius(kMaxRadiusRaw));
    CHECK_EQ(radiusSquaredFromRadius(0x7FFFFFFF), radiusSquaredFromRadius(kMaxRadiusRaw));
    CHECK(((int64_t)kMaxRadiusRaw * kMaxRadiusRaw >> 12) <= 0x7FFFFFFF);
    CHECK(((int64_t)(kMaxRadiusRaw + 1) * (kMaxRadiusRaw + 1) >> 12) > 0x7FFFFFFF);
}

TEST(isqrt_floors) {
    CHECK_EQ(isqrt64(0), 0);
    CHECK_EQ(isqrt64(3), 1);
    CHECK_EQ(isqrt64(4), 2);
    CHECK_EQ(isqrt64(4096ull * 4096ull - 1), 4095);
    CHECK_EQ(isqrt64(4096ull * 4096ull), 4096);
    for (uint64_t v = 5; v < (1ull << 50); v = v * 3 + 7) {
        uint64_t r = isqrt64(v);
        CHECK(r * r <= v && (r + 1) * (r + 1) > v);
    }
}

// Writing back what GetRadius read keeps the same reach, so a script can read,
// adjust and write without drift. Below half a unit the square comes back
// exactly; past it whole raw radii skip some squares, and the one stored may
// be lower but reaches the same distance.
TEST(radius_round_trip) {
    for (int64_t sq = 0; sq < 0x7FFF0000; sq = sq * 3 / 2 + 1) {
        int32_t reach = radiusFromRadiusSquared((int32_t)sq);
        int32_t back = radiusSquaredFromRadius(reach);
        if (reach < 2048) CHECK_EQ(back, sq);
        CHECK(back <= sq);
        CHECK_EQ(radiusFromRadiusSquared(back), reach);
    }
}

int main() { return psxsplash::test::runAll(); }
