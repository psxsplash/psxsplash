#pragma once

#include <stdint.h>

// Splashpack header sizes per format version, free of psyqo so that
// tests/host/test_shadow.cpp can check the mapping natively. splashpack.cpp
// static_asserts its header struct against the newest size.

namespace psxsplash {

// The header has only ever grown by appending, so an older pack is parsed by
// starting the cursor at the size it had back then.
static constexpr uint32_t kSplashpackHeaderSizeV20 = 120;
static constexpr uint32_t kSplashpackHeaderSizeV21 = 128;
static constexpr uint32_t kSplashpackHeaderSizeV22 = 144;
static constexpr uint32_t kSplashpackHeaderSizeV24 = 148;
static constexpr uint32_t kSplashpackHeaderSizeV25 = 156;  // v25..v27
static constexpr uint32_t kSplashpackHeaderSizeV28 = 160;  // + shadowTableOffset
// v31: two words kept for the v29 and v30 header fields (written 0), then
// particleTableOffset.
static constexpr uint32_t kSplashpackHeaderSizeV31 = 172;

inline uint32_t splashpackHeaderSize(uint16_t version) {
    if (version >= 31) return kSplashpackHeaderSizeV31;
    if (version >= 28) return kSplashpackHeaderSizeV28;
    if (version >= 25) return kSplashpackHeaderSizeV25;
    if (version >= 24) return kSplashpackHeaderSizeV24;
    if (version >= 22) return kSplashpackHeaderSizeV22;
    if (version >= 21) return kSplashpackHeaderSizeV21;
    return kSplashpackHeaderSizeV20;
}

}  // namespace psxsplash
