// Host tests for the video mode table (src/screenmode.hh/.cpp), compiled natively.
//
// Every place that used to assume 320x240 (projection centre, UI anchors, tile
// culling, overlay corners) now reads currentScreen(); these pin down what a
// scene's videoMode byte turns into and that the field of view survives the
// switch to 480 lines.

#include "screenmode.hh"

#include "testing.hh"

using namespace psxsplash;

TEST(mode_zero_is_the_old_screen) {
    ScreenMode m = screenModeFor(VIDEO_MODE_320x240);
    CHECK_EQ(m.width, 320);
    CHECK_EQ(m.height, 240);
    CHECK(!m.interlaced);
}

TEST(mode_one_is_640x480_interlaced) {
    ScreenMode m = screenModeFor(VIDEO_MODE_640x480I);
    CHECK_EQ(m.width, 640);
    CHECK_EQ(m.height, 480);
    CHECK(m.interlaced);
}

TEST(unknown_mode_falls_back_to_320x240) {
    ScreenMode m = screenModeFor(7);
    CHECK_EQ(m.width, 320);
    CHECK_EQ(m.height, 240);
    CHECK(!m.interlaced);
}

TEST(projection_h_unchanged_at_240_lines) {
    CHECK_EQ(scaledProjectionH(120, screenModeFor(VIDEO_MODE_320x240)), 120);
    CHECK_EQ(scaledProjectionH(200, screenModeFor(VIDEO_MODE_320x240)), 200);
}

TEST(projection_h_doubles_at_480_lines) {
    CHECK_EQ(scaledProjectionH(120, screenModeFor(VIDEO_MODE_640x480I)), 240);
    CHECK_EQ(scaledProjectionH(77, screenModeFor(VIDEO_MODE_640x480I)), 154);
}

TEST(field_of_view_is_kept) {
    // A point on the right edge of the 320x240 screen (x = H * halfWidth / H)
    // lands on the right edge of the 640x480 one: tan(fov/2) = half / H.
    const ScreenMode lo = screenModeFor(VIDEO_MODE_320x240);
    const ScreenMode hi = screenModeFor(VIDEO_MODE_640x480I);
    const int32_t vx = 160, vz = 120;  // on the edge at H = 120
    CHECK_EQ(vx * scaledProjectionH(120, lo) / vz + lo.width / 2, lo.width);
    CHECK_EQ(vx * scaledProjectionH(120, hi) / vz + hi.width / 2, hi.width);
}

TEST(current_screen_starts_at_320x240_and_follows_set) {
    CHECK_EQ(currentScreen().width, 320);
    CHECK_EQ(currentScreen().height, 240);
    setCurrentScreen(screenModeFor(VIDEO_MODE_640x480I));
    CHECK_EQ(currentScreen().width, 640);
    CHECK_EQ(currentScreen().height, 480);
    CHECK(currentScreen().interlaced);
    setCurrentScreen(screenModeFor(VIDEO_MODE_320x240));
    CHECK_EQ(currentScreen().height, 240);
}

int main() { return psxsplash::test::runAll(); }
