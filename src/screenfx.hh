#pragma once

// Screen effects: fade to and from a colour, flash, camera shake, letterbox
// and a motion trail. Scripts (Lua's Screen table, Blocks) and cutscene tracks
// drive the state in screenfxmath.hh; the three Render paths draw it:
//
//   beginFrame()   after setProjectionH: the shake, as a 2D GTE OFX/OFY offset
//                  (render only, the camera does not move)
//   drawWorld()    before the sprites and the UI: the trail and the letterbox
//                  bars, over the world and the sprites, under the UI
//   drawOverlay()  chained after every text: the fade and the flash, over
//                  everything

#include <stdint.h>

#include "renderer.hh"
#include "screenfxmath.hh"

namespace psxsplash {

class ScreenFx {
  public:
    static ScreenFx& get() { return s_instance; }

    // The display mode. The trail reads the previous frame back from the
    // displayed buffer, so it needs double buffering (off in single-buffer
    // modes such as 640x480 interlaced).
    void setScreen(int16_t width, int16_t height, bool doubleBuffered);
    // Engine init, once: the fade's dither masks into VRAM (960, 456).
    void uploadMasks(psyqo::GPU& gpu);
    int16_t width() const { return m_width; }
    int16_t height() const { return m_height; }

    // Scene load: every effect off.
    void reset();
    void tick(int32_t dt12);

    screenfx::State& state() { return m_state; }

    // Lua and Blocks.
    void trail(int32_t on);

    void beginFrame();
    void drawWorld(Renderer::OT& ot, Renderer::Balloc& balloc, unsigned parity, int32_t otSize);
    void drawOverlay(psyqo::GPU& gpu, Renderer::Balloc& balloc);

  private:
    void drawOverlayPasses(psyqo::GPU& gpu, Renderer::Balloc& balloc);

  public:

  private:
    static ScreenFx s_instance;
    screenfx::State m_state;
    int16_t m_width = 320, m_height = 240;
    bool m_doubleBuffered = true;
    bool m_maskForced = false;  // GPU draws with the mask bit set (trail on)
    bool m_trailWarned = false;
    bool m_offsetSet = false;   // GTE OFX/OFY hold a shake offset
#ifdef PSXSPLASH_PROFILER
    uint32_t m_costTicks = 0, m_costFrames = 0, m_costPeak = 0, m_costFrame = 0, m_ctrlTicks = 0;
    uint16_t m_costStart = 0;
  public:
    // Root counter 2 at system clock / 8 around the effects' CPU work.
    void costBegin();
    void costEnd();
#endif
};

}  // namespace psxsplash
