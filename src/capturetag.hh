#pragma once

#include <stdint.h>

// Capture tags: which object, triangle and ordering-table slot drew each mesh
// primitive, for an editor's frame debugger reading a GPU dump of one frame.
//
// Off by default. A debugger (the editor's framecapture.lua in pcsx-redux)
// turns it on by writing 1 to `enabled` in g_captureTag, which it finds by
// scanning main RAM once for the 8 magic bytes "PSXSPTAG" with `version` == 1
// right after them; no symbol file is needed. The renderer reads `enabled` once
// per frame, at the start of Render*, so a frame is tagged wholly or not at all.
//
// Layout of g_captureTag (little-endian, 4-aligned, 24 bytes):
//   +0   char[8]  magic    "PSXSPTAG"
//   +8   uint32   version  1
//   +12  uint32   enabled  0 = off (default), nonzero = on; written by the debugger
//   +16  uint32   tagged   primitives tagged in the last frame drawn with tagging on
//   +20  uint32   skipped  primitives left untagged in that frame (bump allocator
//                          headroom, see Renderer::appendCaptureTag)
//
// When on, every mesh triangle primitive (static and skinned meshes, and the
// fog overlay pass drawn over a fogged textured triangle) carries 2 extra GP0
// words at the end of its own DMA packet. Both are GP0(00h) NOPs, which leave
// GPUSTAT, the GP1(10h) registers and VRAM unchanged, so the frame draws the
// same; their 24-bit parameters carry the tag:
//   word 1 = 0x00A00000 | kind << 18 | (obj >> 10) << 16 | tri
//   word 2 = 0x00000000 | (obj & 0x3ff) << 14 | slot
// with
//   0xA      bits 23-20 of word 1: marks a capture tag
//   kind     2 bits: 0 = the triangle's primitive, 1 = its fog overlay pass
//   obj      12 bits: index of the game object in the splashpack (0xfff when
//            the index does not fit)
//   tri      16 bits: index of the triangle in that object's triangle list (the
//            unsplit one: every piece of a triangle split against affine warp
//            or clipped to the screen carries the triangle it came from)
//   slot     14 bits: the ordering-table bucket the packet was inserted in
//            (after the insert's clamp; 0x3fff when it does not fit)
// Sprites, tiles, UI, shadows and debug overlays are not tagged.

namespace psxsplash {

struct CaptureTagControl {
    char magic[8];
    uint32_t version;
    uint32_t enabled;
    uint32_t tagged;
    uint32_t skipped;
};

extern volatile CaptureTagControl g_captureTag;

static constexpr uint32_t kCaptureTagMarker = 0x00a00000;
static constexpr unsigned kCaptureTagMain = 0;
static constexpr unsigned kCaptureTagFog = 1;

inline uint32_t captureTagWord1(unsigned kind, uint32_t obj, uint32_t tri) {
    if (obj > 0xfff) obj = 0xfff;
    return kCaptureTagMarker | (kind & 3) << 18 | (obj >> 10) << 16 | (tri & 0xffff);
}
inline uint32_t captureTagWord2(uint32_t obj, uint32_t slot) {
    if (obj > 0xfff) obj = 0xfff;
    if (slot > 0x3fff) slot = 0x3fff;
    return (obj & 0x3ff) << 14 | slot;
}

}  // namespace psxsplash
