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

// The GPU drops a triangle spanning more than 1023x511 pixels and takes
// vertex coordinates -1024..1023. The rasterizer's safe box is that span
// centred on the screen (on OFX/OFY), so it covers the whole screen in either
// mode: -351..672 x -135..376 at 320x240, -191..832 x -15..496 at 640x480.
// Vertices are clamped or clipped to it (triclip.hh, Renderer::clipTriangle).
constexpr int16_t kSafeSpanX = 1023;
constexpr int16_t kSafeSpanY = 511;
struct SafeBox {
    int16_t minX, maxX, minY, maxY;
};
constexpr SafeBox safeBoxFor(const ScreenMode& m) {
    return {int16_t(m.width / 2 - kSafeSpanX / 2), int16_t(m.width / 2 - kSafeSpanX / 2 + kSafeSpanX),
            int16_t(m.height / 2 - kSafeSpanY / 2), int16_t(m.height / 2 - kSafeSpanY / 2 + kSafeSpanY)};
}

const ScreenMode& currentScreen();
void setCurrentScreen(const ScreenMode& m);

// The current screen's safe box: read on every triangle, so a plain global
// that setCurrentScreen keeps in step rather than a call.
namespace detail {
extern SafeBox g_safeBox;
}
inline const SafeBox& safeBox() { return detail::g_safeBox; }

}  // namespace psxsplash
