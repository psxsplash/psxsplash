// Host tests for the sprite math (src/spritemath.cpp), compiled natively.
//
// The rendering needs a PlayStation. This does not — and this is where the bugs
// actually are: off-by-one frame advances, wrap-around on a sheet's grid, and
// yaw sectors that snap to the wrong direction. All of that is arithmetic, so
// all of it is checkable here rather than by squinting at an emulator.

#include "spritemath.hh"

#include "testing.hh"

using namespace psxsplash;
using namespace psxsplash::spritemath;

namespace {

/// 4 columns x 2 rows of 16x16 cells, based at texel (32, 64) in its page.
SpriteSheet makeSheet() {
    SpriteSheet s{};
    s.name = "test";
    s.u0 = 32;
    s.v0 = 64;
    s.cellW = 16;
    s.cellH = 16;
    s.cols = 4;
    s.rows = 2;
    return s;
}

}  // namespace

// ---------------------------------------------------------------------------
// advanceAnim
// ---------------------------------------------------------------------------

TEST(anim_holds_frame_until_duration_elapses) {
    // duration 3 => the frame must survive two ticks and change on the third.
    auto t1 = advanceAnim(0, 0, 4, 3, true);
    CHECK_EQ(t1.frame, 0);
    CHECK_EQ(t1.timer, 1);
    CHECK(!t1.finished);

    auto t2 = advanceAnim(t1.frame, t1.timer, 4, 3, true);
    CHECK_EQ(t2.frame, 0);
    CHECK_EQ(t2.timer, 2);

    auto t3 = advanceAnim(t2.frame, t2.timer, 4, 3, true);
    CHECK_EQ(t3.frame, 1);
    CHECK_EQ(t3.timer, 0);
    CHECK(!t3.finished);
}

TEST(anim_duration_one_advances_every_tick) {
    auto t = advanceAnim(0, 0, 4, 1, true);
    CHECK_EQ(t.frame, 1);
    CHECK_EQ(t.timer, 0);
}

TEST(anim_loops_back_to_zero_and_never_finishes) {
    auto t = advanceAnim(3, 0, 4, 1, true);  // last frame of a 4-frame loop
    CHECK_EQ(t.frame, 0);
    CHECK(!t.finished);
}

TEST(anim_without_loop_holds_last_frame_and_reports_finished) {
    auto t = advanceAnim(3, 0, 4, 1, false);
    CHECK_EQ(t.frame, 3);  // holds, does not wrap
    CHECK(t.finished);
}

TEST(anim_finished_only_fires_at_the_end_not_before) {
    for (uint8_t f = 0; f < 3; f++) {
        auto t = advanceAnim(f, 0, 4, 1, false);
        CHECK(!t.finished);
    }
}

TEST(anim_single_frame_nonlooping_finishes_after_its_duration) {
    auto t1 = advanceAnim(0, 0, 1, 2, false);
    CHECK(!t1.finished);  // still dwelling
    auto t2 = advanceAnim(t1.frame, t1.timer, 1, 2, false);
    CHECK(t2.finished);
}

TEST(anim_zero_duration_advances_instead_of_freezing) {
    // Bad exporter data must degrade to a fast animation, never to a hang.
    auto t = advanceAnim(0, 0, 4, 0, true);
    CHECK_EQ(t.frame, 1);
}

TEST(anim_zero_frames_is_finished_not_a_divide_by_zero) {
    auto t = advanceAnim(0, 0, 0, 4, true);
    CHECK_EQ(t.frame, 0);
    CHECK(t.finished);
}

TEST(anim_full_cycle_takes_frameCount_times_duration_ticks) {
    // The property that matters to a designer: a 4-frame anim held 3 frames each
    // returns to frame 0 after exactly 12 ticks.
    uint8_t frame = 0, timer = 0;
    int ticks = 0;
    for (int i = 0; i < 12; i++) {
        auto t = advanceAnim(frame, timer, 4, 3, true);
        frame = t.frame;
        timer = t.timer;
        ticks++;
    }
    CHECK_EQ(ticks, 12);
    CHECK_EQ(frame, 0);
    CHECK_EQ(timer, 0);
}

// ---------------------------------------------------------------------------
// frameToUV
// ---------------------------------------------------------------------------

TEST(uv_frame_zero_is_the_sheet_origin) {
    SpriteSheet s = makeSheet();
    uint8_t u, v;
    frameToUV(s, 0, u, v);
    CHECK_EQ(u, 32);
    CHECK_EQ(v, 64);
}

TEST(uv_walks_across_the_row) {
    SpriteSheet s = makeSheet();
    uint8_t u, v;
    frameToUV(s, 2, u, v);
    CHECK_EQ(u, 32 + 2 * 16);
    CHECK_EQ(v, 64);
}

TEST(uv_wraps_to_the_next_row_after_the_last_column) {
    SpriteSheet s = makeSheet();
    uint8_t u, v;
    frameToUV(s, 4, u, v);  // cols == 4, so frame 4 starts row 1
    CHECK_EQ(u, 32);
    CHECK_EQ(v, 64 + 16);
}

TEST(uv_last_cell_of_the_grid) {
    SpriteSheet s = makeSheet();
    uint8_t u, v;
    frameToUV(s, 7, u, v);
    CHECK_EQ(u, 32 + 3 * 16);
    CHECK_EQ(v, 64 + 16);
}

TEST(uv_zero_cols_does_not_divide_by_zero) {
    SpriteSheet s = makeSheet();
    s.cols = 0;
    uint8_t u, v;
    frameToUV(s, 3, u, v);  // must not crash; degenerates to one cell per row
    CHECK_EQ(u, 32);
    CHECK_EQ(v, (uint8_t)(64 + 3 * 16));
}

// ---------------------------------------------------------------------------
// facingFromYaw
// ---------------------------------------------------------------------------

TEST(facing_yaw_zero_is_direction_zero) {
    CHECK_EQ(facingFromYaw(0, 8), 0);
}

TEST(facing_rounds_to_nearest_sector_not_down) {
    // With 8 directions each sector is 256 raw units, and direction d owns the
    // half-open interval [d*256 - 128, d*256 + 128) — centred on its own angle
    // rather than starting there. Ties round up, so both edges of direction 0
    // resolve consistently: +128 leaves it upward, and -128 enters it.
    CHECK_EQ(facingFromYaw(127, 8), 0);
    CHECK_EQ(facingFromYaw(128, 8), 1);
    CHECK_EQ(facingFromYaw(-128, 8), 0);
    CHECK_EQ(facingFromYaw(-129, 8), 7);  // one below the edge falls back a sector
}

TEST(facing_quarter_turn_with_four_directions) {
    // Full turn is 2048, so a quarter turn is 512.
    CHECK_EQ(facingFromYaw(512, 4), 1);
    CHECK_EQ(facingFromYaw(1024, 4), 2);
    CHECK_EQ(facingFromYaw(1536, 4), 3);
}

TEST(facing_wraps_at_a_full_turn) {
    CHECK_EQ(facingFromYaw(2048, 8), 0);
    CHECK_EQ(facingFromYaw(2048 + 512, 8), facingFromYaw(512, 8));
}

TEST(facing_handles_negative_yaw) {
    // C's % keeps the dividend's sign; a naive implementation returns garbage
    // or indexes out of the animation table here.
    CHECK_EQ(facingFromYaw(-512, 4), 3);
    CHECK_EQ(facingFromYaw(-2048, 8), 0);
    CHECK_EQ(facingFromYaw(-4096 - 512, 4), 3);
}

TEST(facing_is_always_in_range_for_every_yaw) {
    // The result indexes an animation table, so an out-of-range value is a
    // wild read. Sweep a full turn either side at every direction count.
    for (uint8_t dirs = 1; dirs <= 16; dirs++) {
        for (int32_t yaw = -4096; yaw <= 4096; yaw += 7) {
            uint8_t d = facingFromYaw(yaw, dirs);
            if (d >= dirs) {
                CHECK_EQ(d, 0);  // reports the offending value
                return;
            }
        }
    }
    CHECK(true);
}

TEST(facing_zero_directions_does_not_divide_by_zero) {
    CHECK_EQ(facingFromYaw(500, 0), 0);
}

int main() { return psxsplash::test::runAll(); }
