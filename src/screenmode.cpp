#include "screenmode.hh"

namespace psxsplash {

namespace {
ScreenMode s_screen = screenModeFor(VIDEO_MODE_320x240);
}

SafeBox detail::g_safeBox = safeBoxFor(screenModeFor(VIDEO_MODE_320x240));

const ScreenMode& currentScreen() { return s_screen; }
void setCurrentScreen(const ScreenMode& m) {
    s_screen = m;
    detail::g_safeBox = safeBoxFor(m);
}

}  // namespace psxsplash
