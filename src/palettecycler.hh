#pragma once

#include <stdint.h>

#include "palettecycle.hh"
#include "renderer.hh"

namespace psxsplash {

/// Rotates colour ranges of the scene's CLUTs (see palettecycle.hh for the pack record).
///
/// The base words stay in the resident splashpack; the cycler only keeps per-range time and step.
/// When a step changes, the next frame carries one VRAM upload per changed CLUT, covering the
/// even-widened span of its ranges, followed by a texture cache flush, head-inserted into the
/// far ordering table bucket after everything else so the GPU writes the CLUT before it draws
/// anything of that frame. A blocking upload from the game tick would race the previous frame's
/// chain, which is still being sent then.
class PaletteCycler {
  public:
    static constexpr int kMaxCluts = 16;

    void clear() { m_count = 0; }

    /// One CLUT table entry of the splashpack. A bad record logs one line and is skipped.
    void addClut(const uint8_t* pack, uint32_t packSize, uint16_t index, uint32_t offset, uint16_t pad,
                 uint16_t clutX16, uint16_t clutY, uint16_t length);

    void tick(int32_t dt12);

    /// Called by the renderer once all of a frame's primitives are in the ordering table.
    void emit(Renderer::OT& ot, Renderer::Balloc& balloc, int32_t farBucket);

    int count() const { return m_count; }

  private:
    struct Clut {
        palcycle::Record rec;
        palcycle::Span span;
        int16_t x, y;
        bool dirty;
        uint32_t acc[palcycle::kMaxRanges];
        uint16_t step[palcycle::kMaxRanges];
    };
    Clut m_cluts[kMaxCluts];
    int m_count = 0;
};

}  // namespace psxsplash
