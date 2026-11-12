// Host tests for stick walking and turning (src/movemath.hh), compiled natively.
//
// Opposite stick directions must cover the same distance, and the distance over
// any run of ticks must be the exact product rounded once, whatever the facing
// and however small the per-tick steps are.

#include <initializer_list>
#include <stdint.h>
#include <stdlib.h>

#include "movemath.hh"

#include "testing.hh"

using namespace psxsplash;
using namespace psxsplash::movemath;

namespace {

// dt12 as a 30 fps engine sees it under load: never a whole frame.
const int32_t kDts[] = {5874, 5803, 5402, 4270, 5906, 4096, 8753};
constexpr int kNdt = sizeof(kDts) / sizeof(kDts[0]);

struct Walked {
    int64_t x, z, dt;
};

Walked walkFor(int ticks, int32_t sx, int32_t sy, int32_t speed, int32_t sinY, int32_t cosY) {
    Carry c;
    Walked w{0, 0, 0};
    for (int i = 0; i < ticks; i++) {
        int32_t dt = kDts[i % kNdt], dx, dz;
        walk(sx, sy, speed, dt, sinY, cosY, c, dx, dz);
        w.x += dx;
        w.z += dz;
        w.dt += dt;
    }
    return w;
}

int64_t floorDiv(int64_t num, int shift) { return num >> shift; }

}  // namespace

TEST(opposite_directions_walk_the_same_distance) {
    // The reported case: D-pad at +-127, the courtyard's 4-unit speed, facing +Z.
    for (int32_t speed : {4, 5, 41, 410}) {
        Walked r = walkFor(21, 127, 0, speed, 0, 4096), l = walkFor(21, -127, 0, speed, 0, 4096);
        Walked f = walkFor(21, 0, -127, speed, 0, 4096), b = walkFor(21, 0, 127, speed, 0, 4096);
        CHECK(llabs(r.x + l.x) <= 1);
        CHECK(llabs(f.z + b.z) <= 1);
        CHECK(llabs(r.x - f.z) <= 1);
        CHECK_EQ(r.z, 0);
        CHECK_EQ(f.x, 0);
    }
}

TEST(each_axis_is_the_exact_product_rounded_once) {
    // sin/cos pairs around the circle, including ones whose per-tick step is under a unit.
    const int32_t trig[][2] = {{0, 4096}, {4096, 0}, {0, -4096}, {-4096, 0}, {2896, 2896}, {-2896, 2896},
                               {1567, 3784}, {-3784, -1567}, {71, 4095}, {-4095, 71}};
    const int32_t sticks[][2] = {{127, 0}, {-127, 0}, {0, 127}, {0, -127}, {90, -90}, {-128, 50}, {49, 0}};
    for (auto& t : trig)
        for (auto& s : sticks)
            for (int32_t speed : {1, 4, 7, 300}) {
                Carry c;
                int64_t x = 0, z = 0, fwdSum = 0, rightSum = 0;
                for (int i = 0; i < 200; i++) {
                    int32_t dt = kDts[i % kNdt], dx, dz;
                    walk(s[0], s[1], speed, dt, t[0], t[1], c, dx, dz);
                    x += dx;
                    z += dz;
                    fwdSum += -(int64_t)s[1] * speed * dt;
                    rightSum += (int64_t)s[0] * speed * dt;
                    int64_t refX = floorDiv(t[0] * fwdSum + t[1] * rightSum, MOVE_SHIFT);
                    int64_t refZ = floorDiv(t[1] * fwdSum - t[0] * rightSum, MOVE_SHIFT);
                    if (!CHECK_EQ(x, refX)) return;
                    if (!CHECK_EQ(z, refZ)) return;
                }
            }
}

TEST(walk_directions_match_the_facing) {
    // Facing +X (sin 1, cos 0): forward is +X, right strafe is -Z.
    Walked f = walkFor(10, 0, -127, 40, 4096, 0), r = walkFor(10, 127, 0, 40, 4096, 0);
    CHECK(f.x > 0);
    CHECK_EQ(f.z, 0);
    CHECK(r.z < 0);
    CHECK_EQ(r.x, 0);
}

TEST(largest_inputs_do_not_overflow) {
    Carry c;
    int32_t dx, dz;
    walk(-128, -128, 65535, 16384, 4096, 4096, c, dx, dz);
    // Forward and left strafe cancel in X and add in Z: 128 * 65535 * 16384 * 4096 * 2 / 2^31.
    CHECK_EQ(dx, 0);
    CHECK_EQ(dz, (int64_t)128 * 65535 * 16384 * 4096 * 2 >> MOVE_SHIFT);
}

TEST(turning_left_and_right_turn_the_same_angle) {
    // rotSpeed 0.02 pi is 20 angle units; L1/R1 are +-90, the stick +-127.
    for (int32_t stick : {90, 127, 49}) {
        int64_t cl = 0, cr = 0, left = 0, right = 0, sum = 0;
        for (int i = 0; i < 300; i++) {
            int32_t dt = kDts[i % kNdt];
            right += turn(stick, 20, dt, cr);
            left += turn(-stick, 20, dt, cl);
            sum += (int64_t)stick * 20 * dt;
            if (!CHECK_EQ(right, floorDiv(sum, TURN_SHIFT))) return;
            if (!CHECK(llabs(right + left) <= 1)) return;
        }
    }
}

int main() { return psxsplash::test::runAll(); }
