#pragma once

#include <stdint.h>

// Per-element UI blending, free of psyqo so tests/host/test_uiblend.cpp
// compiles this exact code natively.
//
// The PS1 has no alpha channel. A primitive is either opaque or drawn with one
// of four fixed blends of the pixel already in VRAM (B) and the primitive (F):
//
//   Half        B/2 + F/2
//   Add         B + F        (saturates at 31 per 5-bit channel)
//   Subtract    B - F        (floors at 0)
//   AddQuarter  B + F/4
//
// A canvas element stores its blend in bits 1-3 of the element flags byte
// (bit 0 is "visible"). 0 is opaque, 1-4 are the four modes in GPU order, so
// a splashpack written before blending existed (those bits zero) reads back as
// opaque, and an engine that predates blending ignores the bits and draws the
// element opaque. Codes 5-7 are unused and read as opaque.

namespace psxsplash {

enum class UIBlend : uint8_t {
    Opaque = 0,
    Half = 1,
    Add = 2,
    Subtract = 3,
    AddQuarter = 4,
};

namespace uiblend {

constexpr uint8_t FLAGS_SHIFT = 1;
constexpr uint8_t FLAGS_MASK = 0x07;

/// The blend stored in an element flags byte.
constexpr UIBlend fromFlags(uint8_t flags) {
    uint8_t code = (flags >> FLAGS_SHIFT) & FLAGS_MASK;
    return code <= 4 ? static_cast<UIBlend>(code) : UIBlend::Opaque;
}

/// The GPU semi-transparency mode (tpage bits 5-6) for a blend that is not
/// Opaque: 0 = B/2+F/2, 1 = B+F, 2 = B-F, 3 = B+F/4.
constexpr uint8_t gpuMode(UIBlend b) { return static_cast<uint8_t>(b) - 1; }

/// What the GPU writes for one 5-bit channel, B already in VRAM, F the
/// element's colour. Used by the tests to pin the mode numbering to the math.
constexpr uint8_t blendChannel(UIBlend b, uint8_t back, uint8_t front) {
    int r = front;
    switch (b) {
        case UIBlend::Opaque: r = front; break;
        case UIBlend::Half: r = (back + front) / 2; break;
        case UIBlend::Add: r = back + front; break;
        case UIBlend::Subtract: r = back - front; break;
        case UIBlend::AddQuarter: r = back + front / 4; break;
    }
    if (r < 0) r = 0;
    if (r > 31) r = 31;
    return static_cast<uint8_t>(r);
}

}  // namespace uiblend
}  // namespace psxsplash
