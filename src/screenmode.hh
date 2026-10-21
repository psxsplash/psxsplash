#pragma once

#include <stdint.h>

namespace psxsplash {

// A scene's video mode, the splashpack header byte `videoMode`.
enum : uint8_t {
    VIDEO_MODE_320x240 = 0,   // progressive, two buffers at y 0 and 256
    VIDEO_MODE_640x480I = 1,  // interlaced, one buffer at y 0
};

// The size of the screen being drawn. Renderer::SetVideoMode is the only
// writer; everything that lays out, projects or culls in screen pixels reads
// currentScreen() instead of assuming 320x240.
struct ScreenMode {
    int16_t width;
    int16_t height;
    bool interlaced;
};

// Unknown modes read as 320x240, so a pack from a newer editor still runs.
constexpr ScreenMode screenModeFor(uint8_t videoMode) {
    return videoMode == VIDEO_MODE_640x480I ? ScreenMode{640, 480, true} : ScreenMode{320, 240, false};
}

// Projection distances (Camera::GetProjectionH, cutscene H tracks, Lua
// Camera.SetH) are authored for 240 lines. Scaling H with the height keeps the
// field of view, so a scene frames the same at 640x480 as at 320x240.
constexpr int32_t kReferenceScreenLines = 240;
constexpr int32_t scaledProjectionH(int32_t authoredH, const ScreenMode& m) {
    return authoredH * m.height / kReferenceScreenLines;
}

const ScreenMode& currentScreen();
void setCurrentScreen(const ScreenMode& m);

}  // namespace psxsplash
