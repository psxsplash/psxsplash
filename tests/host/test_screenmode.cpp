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

// The rasterizer safe box: 320x240 keeps the box the engine always had; every
// mode's box covers its screen, spans exactly the GPU's 1023x511 and stays
// inside its -1024..1023 vertex range.
TEST(safe_box_is_the_old_one_at_320x240) {
    SafeBox b = safeBoxFor(screenModeFor(VIDEO_MODE_320x240));
    CHECK_EQ(b.minX, -351);
    CHECK_EQ(b.maxX, 672);
    CHECK_EQ(b.minY, -135);
    CHECK_EQ(b.maxY, 376);
}

TEST(safe_box_covers_every_screen_within_gpu_limits) {
    const uint8_t modes[] = {VIDEO_MODE_320x240, VIDEO_MODE_640x480I};
    for (uint8_t mode : modes) {
        ScreenMode m = screenModeFor(mode);
        SafeBox b = safeBoxFor(m);
        CHECK(b.minX < 0 && b.maxX >= m.width && b.minY < 0 && b.maxY >= m.height);
        CHECK_EQ(b.maxX - b.minX, 1023);
        CHECK_EQ(b.maxY - b.minY, 511);
        CHECK(b.minX >= -1024 && b.maxX <= 1023 && b.minY >= -1024 && b.maxY <= 1023);
    }
    SafeBox hi = safeBoxFor(screenModeFor(VIDEO_MODE_640x480I));
    CHECK_EQ(hi.minY, -15);
    CHECK_EQ(hi.maxY, 496);
}

TEST(safe_box_follows_set_current_screen) {
    CHECK_EQ(safeBox().maxY, 376);
    setCurrentScreen(screenModeFor(VIDEO_MODE_640x480I));
    CHECK_EQ(safeBox().maxY, 496);
    CHECK_EQ(safeBox().minX, -191);
    setCurrentScreen(screenModeFor(VIDEO_MODE_320x240));
    CHECK_EQ(safeBox().maxY, 376);
}

int main() { return psxsplash::test::runAll(); }
