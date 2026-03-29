#pragma once

namespace psxsplash {

inline bool streq(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

}  // namespace psxsplash
