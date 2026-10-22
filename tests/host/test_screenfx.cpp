// Host tests for the screen effects state (src/screenfxmath.hh): the ramps,
// what Lua and cutscene tracks write, and what the renderer reads back.

#include "screenfxmath.hh"

#include "testing.hh"

using namespace psxsplash::screenfx;

static constexpr int32_t kFrame = 4096;  // dt12 of one 30 fps frame

TEST(ramp_reaches_target_in_frames) {
    Ramp r;
    r.start(255, 30);
    CHECK(r.running());
    for (int i = 0; i < 15; i++) r.tick(kFrame);
    CHECK_EQ(r.value, 127);  // half way: 255 * 128 / 256
    for (int i = 0; i < 14; i++) r.tick(kFrame);
    CHECK(r.running());
    r.tick(kFrame);
    CHECK_EQ(r.value, 255);
    CHECK(!r.running());
}

TEST(ramp_is_monotonic_and_frame_rate_independent) {
    // 60 fps (half frames) lands where 30 fps does.
    Ramp a, b;
    a.start(200, 20);
    b.start(200, 20);
    int prev = 0, bad = 0;
    for (int i = 0; i < 10; i++) {
        a.tick(kFrame);
        b.tick(kFrame / 2);
        b.tick(kFrame / 2);
        if (a.value < prev) bad++;
        prev = a.value;
        if (a.value != b.value) bad++;
    }
    CHECK_EQ(bad, 0);
}

TEST(ramp_zero_frames_is_immediate_and_down_works) {
    Ramp r;
    r.start(100, 0);
    CHECK_EQ(r.value, 100);
    CHECK(!r.running());
    r.start(0, 10);
    r.tick(kFrame * 5);
    CHECK_EQ(r.value, 50);
    r.tick(kFrame * 100);  // a long frame overshoots, clamps to the target
    CHECK_EQ(r.value, 0);
    r.start(30, 1000000);  // clamped to kMaxFrames, no overflow
    CHECK_EQ(r.frames, kMaxFrames);
    r.tick(kFrame * kMaxFrames);
    CHECK_EQ(r.value, 30);
}

TEST(fade_out_then_in_and_is_fading) {
    State s;
    CHECK(!s.isFading());
    s.fadeOut(255, 0, 0, 10);
    CHECK(s.isFading());
    for (int i = 0; i < 10; i++) s.tick(kFrame);
    CHECK(!s.isFading());  // done: faded, but no longer fading
    CHECK_EQ(s.fade.value, 255);
    s.fadeIn(4);
    CHECK(s.isFading());
    for (int i = 0; i < 4; i++) s.tick(kFrame);
    CHECK_EQ(s.fade.value, 0);
    CHECK(s.idle());
}

TEST(full_fade_is_exactly_the_colour) {
    State s;
    s.fadeOut(10, 200, 77, 0);
    State::FadePasses p = s.fadePasses();
    CHECK_EQ(p.dither, kDitherLevels);  // 50/50 everywhere, then
    CHECK_EQ(p.sub, 255);               // everything subtracted, then
    CHECK_EQ(p.addR, 10);               // exactly the colour added
    CHECK_EQ(p.addG, 200);
    CHECK_EQ(p.addB, 77);
}

TEST(fade_passes_are_continuous) {
    State s;
    s.fadeOut(255, 0, 0, 0);
    s.fade.set(0);
    CHECK_EQ(s.fadePasses().dither, 0);
    CHECK_EQ(s.fadePasses().sub, 0);
    // The dither level climbs one step at a time to 16 at 128, where the
    // subtract/add part starts from 0: no step anywhere is more than one level
    // or a few units of subtraction.
    int prevDither = 0, prevSub = 0, bad = 0;
    for (int a = 0; a <= 255; a++) {
        s.fade.set(a);
        State::FadePasses p = s.fadePasses();
        if (p.dither < prevDither || p.dither > prevDither + 1) bad++;
        if (p.sub < prevSub || p.sub > prevSub + 3) bad++;
        if (a < 128 && p.sub != 0) bad++;
        prevDither = p.dither;
        prevSub = p.sub;
    }
    CHECK_EQ(bad, 0);
    s.fade.set(127);
    CHECK_EQ(s.fadePasses().dither, 16);  // rounds up into the blend
    s.fade.set(128);
    CHECK_EQ(s.fadePasses().dither, 16);
    CHECK_EQ(s.fadePasses().sub, 0);
}

TEST(dither_masks_cover_level_sixteenths) {
    uint16_t m[kMaskTexW / 4 * kMaskTexH];
    buildDitherMasks(m);
    for (int level = 1; level <= kDitherLevels; level++) {
        int on = 0, bad = 0;
        for (int y = 0; y < kMaskTexH; y++)
            for (int x = 0; x < 8; x++) {
                const int u = (level - 1) * 8 + x;
                const bool texel = (m[y * (kMaskTexW / 4) + u / 4] >> ((u & 3) * 4)) & 15;
                on += texel;
                // Every 4x4 cell of the 8x8 tile is the same pattern.
                const int u2 = (level - 1) * 8 + (x ^ 4);
                const bool other = (m[(y ^ 4) * (kMaskTexW / 4) + u2 / 4] >> ((u2 & 3) * 4)) & 15;
                if (texel != other) bad++;
            }
        CHECK_EQ(on, level * 4);  // level/16 of the 64 texels
        CHECK_EQ(bad, 0);
    }
    // Each level keeps every texel of the one below: the fade only adds pixels.
    int lost = 0;
    for (int level = 2; level <= kDitherLevels; level++)
        for (int y = 0; y < 4; y++)
            for (int x = 0; x < 4; x++)
                if (ditherOn(level - 1, x, y) && !ditherOn(level, x, y)) lost++;
    CHECK_EQ(lost, 0);
}

TEST(flash_decays_to_nothing) {
    State s;
    s.startFlash(255, 128, 0, 8);
    uint8_t r, g, b;
    s.flashAdd(r, g, b);
    CHECK_EQ(r, 255);
    CHECK_EQ(g, 128);
    CHECK_EQ(b, 0);
    int prev = 255, bad = 0;
    for (int i = 0; i < 8; i++) {
        s.tick(kFrame);
        s.flashAdd(r, g, b);
        if (r > prev) bad++;
        prev = r;
    }
    CHECK_EQ(bad, 0);
    CHECK_EQ(r, 0);
    CHECK(s.idle());
}

TEST(shake_offsets_bounded_moving_deterministic) {
    State s;
    s.startShake(12, 100);
    int bad = 0, same = 0;
    int32_t pdx = 0, pdy = 0;
    for (int i = 0; i < 50; i++) {
        s.tick(kFrame);
        int32_t dx, dy;
        s.shakeOffsets(dx, dy);
        const int amp = s.shake.value;
        if (dx > amp || dx < -amp || dy > amp || dy < -amp) bad++;
        if (amp >= 2 && (dx == 0 || dy == 0)) bad++;  // always off centre while shaking
        if (i > 0 && dx == pdx && dy == pdy) same++;
        pdx = dx;
        pdy = dy;
    }
    CHECK_EQ(bad, 0);
    CHECK_EQ(same, 0);  // x alternates sides every frame
    int32_t a1, b1, a2, b2;
    shakeOffset(1234, 9, a1, b1);
    shakeOffset(1234, 9, a2, b2);
    CHECK_EQ(a1, a2);
    CHECK_EQ(b1, b2);
    s.startShake(500, 10);
    CHECK_EQ(s.shake.value, 64);  // capped
    for (int i = 0; i < 10; i++) s.tick(kFrame);
    int32_t dx, dy;
    s.shakeOffsets(dx, dy);
    CHECK_EQ(dx, 0);
    CHECK_EQ(dy, 0);
}

TEST(letterbox_clamps_to_half_screen) {
    State s;
    s.startLetterbox(500, 0, 240);
    CHECK_EQ(s.letterbox.value, 120);
    s.startLetterbox(-5, 0, 240);
    CHECK_EQ(s.letterbox.value, 0);
    s.startLetterbox(40, 4, 240);
    s.tick(kFrame * 2);
    CHECK_EQ(s.letterbox.value, 20);
}

TEST(trail_is_on_or_off) {
    State s;
    s.setTrail(1);
    CHECK_EQ(s.trail, 1);
    s.setTrail(255);
    CHECK_EQ(s.trail, 1);
    s.setTrail(-3);
    CHECK_EQ(s.trail, 1);
    s.setTrail(0);
    CHECK_EQ(s.trail, 0);
    CHECK_EQ(State::kTrailGrey, 128);  // unmodulated previous frame
}

TEST(stopped_cutscene_resets_only_what_it_drove) {
    State s;
    s.startLetterbox(30, 0, 240);  // from a script
    s.driveFade(200);
    s.driveFadeColor(255, 0, 300);
    CHECK_EQ(s.fadeB, 255);
    s.driveShake(9);
    s.driveTrail(1);
    s.resetDriven();
    CHECK_EQ(s.fade.value, 0);
    CHECK_EQ(s.fadeR, 0);
    CHECK_EQ(s.shake.value, 0);
    CHECK_EQ(s.trail, 0);
    CHECK_EQ(s.letterbox.value, 30);  // not driven: kept
    CHECK_EQ(s.driven, 0);
    s.driveLetterbox(10, 240);
    s.resetDriven();
    CHECK_EQ(s.letterbox.value, 0);
}

TEST(idle_only_when_nothing_shows) {
    State s;
    CHECK(s.idle());
    s.setTrail(1);
    CHECK(!s.idle());
    s.setTrail(0);
    s.startLetterbox(0, 5, 240);  // already 0: nothing to animate
    CHECK(s.idle());
    s.fadeOut(0, 0, 0, 3);  // running from 0
    CHECK(!s.idle());
    s.reset();
    CHECK(s.idle());
}

int main() { return psxsplash::test::runAll(); }
