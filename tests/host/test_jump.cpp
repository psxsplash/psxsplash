// Host tests for the player's vertical motion (src/jumpmath.hh), compiled natively.
//
// The pack values are made the way both exporters make them: jump velocity
// sqrt(2 g h) / gte * 4096 per second, gravity g / gte * 4096 per second^2.
// A jump must peak at h, rise for sqrt(2 h / g) and fall back as long.

#include <math.h>
#include <stdio.h>

#include "jumpmath.hh"

#include "testing.hh"

using namespace psxsplash;
using namespace psxsplash::jumpmath;

namespace {

struct Pack {
    int32_t jumpVelocity, gravity;
};

Pack exportPack(double heightM, double gravityM, double gte) {
    Pack p;
    p.jumpVelocity = (int32_t)lround(sqrt(2.0 * gravityM * heightM) / gte * 4096.0);
    p.gravity = (int32_t)lround(gravityM / gte * 4096.0);
    return p;
}

struct Arc {
    int32_t apexRaw;   // highest point above the start (20.12)
    int32_t riseDt12;  // dt12 from launch to the tick the velocity turns downward
    int32_t fallDt12;  // dt12 from then back to the start height
};

// Jumps from y = 0 and steps until back at or below it, with dt12 cycling over the given steps.
Arc simulate(const Pack& p, const int32_t* dts, int ndt) {
    VerticalMotion m;
    launch(m, p.jumpVelocity);
    int32_t terminal = terminalVelocity(p.gravity, p.jumpVelocity);
    int32_t y = 0, t = 0, apexY = 0, apexT = -1;
    for (int i = 0; i < 100000; i++) {
        int32_t dt = dts[i % ndt];
        y += step(m, p.gravity, terminal, dt);
        t += dt;
        if (y < apexY) apexY = y;
        // Positions are whole 20.12 units, so the top of the arc can sit on one Y for
        // several ticks; the velocity says when it turned.
        if (apexT < 0 && m.velocity >= 0) apexT = t;
        if (y >= 0) break;
    }
    return {-apexY, apexT, t - apexT};
}

bool within(double got, double want, double frac) { return fabs(got - want) <= fabs(want) * frac; }

void checkHeight(double h, const int32_t* dts, int ndt, double apexTol) {
    const double gte = 100.0, g = 20.0;
    Pack p = exportPack(h, g, gte);
    Arc a = simulate(p, dts, ndt);
    double apexM = a.apexRaw * gte / 4096.0;
    double riseS = a.riseDt12 / double(DT12_PER_SECOND), fallS = a.fallDt12 / double(DT12_PER_SECOND);
    double want = sqrt(2.0 * h / g);
    // The apex and landing are seen at tick boundaries, so timings are good to one tick.
    double tick = 0;
    for (int i = 0; i < ndt; i++) tick = fmax(tick, dts[i] / double(DT12_PER_SECOND));
    printf("    h=%.2f m: apex %.3f m rise %.3f s fall %.3f s (want %.3f s)\n", h, apexM, riseS, fallS, want);
    CHECK(within(apexM, h, apexTol));
    CHECK(fabs(riseS - want) <= tick);
    CHECK(fabs(fallS - want) <= tick);
}

}  // namespace

TEST(jump_peaks_at_exported_height_at_30fps) {
    const int32_t dts[] = {4096};
    checkHeight(0.5, dts, 1, 0.03);
    checkHeight(2.0, dts, 1, 0.02);
    checkHeight(4.0, dts, 1, 0.02);
}

TEST(jump_peaks_at_exported_height_at_uneven_frame_rates) {
    // dt12 as measured on a busy scene in Redux: two to four vsyncs a tick.
    const int32_t dts[] = {8753, 7832, 16384, 8367, 15823, 7880};
    checkHeight(0.5, dts, 6, 0.05);
    checkHeight(2.0, dts, 6, 0.05);
    checkHeight(4.0, dts, 6, 0.05);
}

TEST(fall_reaches_terminal_velocity_after_two_and_a_half_seconds) {
    Pack p = exportPack(2.0, 20.0, 100.0);
    int32_t terminal = terminalVelocity(p.gravity, p.jumpVelocity);
    CHECK_EQ(terminal, p.gravity * 5 / 2);  // 50 m/s at 20 m/s^2
    VerticalMotion m;
    int32_t y = 0;
    for (int i = 0; i < 74; i++) y += step(m, p.gravity, terminal, 4096);  // 2.47 s
    CHECK(m.velocity < terminal);
    for (int i = 0; i < 2; i++) y += step(m, p.gravity, terminal, 4096);
    CHECK_EQ(m.velocity, terminal);
    // Free fall to the cap covers g t^2 / 2 = 62.5 m.
    CHECK(within(y * 100.0 / 4096.0, 62.5 + 50.0 * (76 / 30.0 - 2.5), 0.02));
}

TEST(terminal_velocity_never_cuts_a_jumps_own_fall) {
    // Low gravity, tall jump: the launch speed is above 2.5 s of gravity.
    Pack p = exportPack(20.0, 1.6, 100.0);
    CHECK_EQ(terminalVelocity(p.gravity, p.jumpVelocity), p.jumpVelocity);
    const int32_t dts[] = {4096};
    Arc a = simulate(p, dts, 1);
    CHECK(within(a.fallDt12, a.riseDt12, 0.02));
}

TEST(step_matches_a_64bit_reference_up_to_the_largest_inputs) {
    // u16 gravity and jump velocity at their maxima, dt12 up to the 4-frame cap.
    const int32_t gs[] = {1, 819, 65535}, js[] = {0, 366, 65535}, dts[] = {1, 4095, 4096, 8753, 16383, 16384};
    for (int32_t g : gs)
        for (int32_t j : js) {
            int32_t terminal = terminalVelocity(g, j);
            VerticalMotion m;
            launch(m, j);
            int64_t refV = -j, refVNum = 0, refYNum = 0, y = 0;  // reference keeps exact numerators
            for (int i = 0; i < 400; i++) {
                int32_t dt = dts[i % 6];
                y += step(m, g, terminal, dt);
                int64_t v0 = refV;
                refVNum += (int64_t)g * dt;
                int64_t v1 = -j + refVNum / DT12_PER_SECOND;
                if (v1 >= terminal) {
                    v1 = terminal;
                    refVNum = (int64_t)(terminal + j) * DT12_PER_SECOND;
                }
                refV = v1;
                refYNum += (v0 + v1) * dt;
                if (!CHECK_EQ(m.velocity, refV)) return;
                if (!CHECK(llabs(y - refYNum / (2 * DT12_PER_SECOND)) <= 1)) return;
            }
        }
}

TEST(stop_and_launch_clear_carries) {
    VerticalMotion m;
    step(m, 819, 2048, 1000);
    CHECK(m.velRem != 0 || m.posRem != 0);
    stop(m);
    CHECK_EQ(m.velocity, 0);
    CHECK_EQ(m.velRem, 0);
    CHECK_EQ(m.posRem, 0);
    launch(m, 366);
    CHECK_EQ(m.velocity, -366);
}

int main() { return psxsplash::test::runAll(); }
