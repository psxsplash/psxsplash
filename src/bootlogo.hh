#pragma once

#include <psyqo/gpu.hh>

namespace psxsplash {

// The psxsplash logo animation shown once at boot, before the first scene
// loads: the SPLASH letters drop in and bounce, then PSX lands and tips onto
// the slope. Built only with the bootlogo engine feature.
class BootLogo {
  public:
    // Uploads the logo texture and CLUTs. Uses VRAM that the first scene
    // overwrites, so the animation has to finish before loadScene().
    void start(psyqo::GPU& gpu);
    // Draws one frame. Returns false once the animation is over.
    bool frame(psyqo::GPU& gpu);

  private:
    uint32_t m_startUs = 0;
};

}  // namespace psxsplash
