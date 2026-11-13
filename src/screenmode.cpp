#include "screenmode.hh"

namespace psxsplash {

ScreenMode detail::g_screen = screenModeFor(VIDEO_MODE_320x240);

SafeBox detail::g_safeBox = safeBoxFor(screenModeFor(VIDEO_MODE_320x240));

void setCurrentScreen(const ScreenMode& m) {
    detail::g_screen = m;
    detail::g_safeBox = safeBoxFor(m);
}

}  // namespace psxsplash
