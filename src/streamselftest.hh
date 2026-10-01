#pragma once

#include <stdint.h>

namespace psxsplash {

// Build with STREAMTEST=1. See streamselftest.cpp.
struct StreamSelfTest {
    static void Start(const char* filename, const uint8_t* data, int size);
};

}  // namespace psxsplash
