#pragma once

#include <stddef.h>
#include <stdint.h>

// A movie's subtitles from its .SRT, free of psyqo so that tests/host/test_srt.cpp compiles this exact code
// natively. A file is blocks of an optional number, then "00:00:01,500 --> 00:00:04,000", then the text, then
// a blank line; CRLF and a UTF-8 BOM are taken as they come.

namespace psxsplash::srt {

constexpr unsigned c_text = 80;
struct Subtitle {
    uint32_t fromMs, toMs;  // from the first picture
    char text[c_text];      // its rows, apart by \n
};

enum class Problem {
    LeftOut,  // no time line and text, or no room: not kept
    Wide,     // more than two rows of 40 characters: kept, the rest off screen
};

// The time lines in the file: room enough for every subtitle it can hold.
unsigned count(const char* data, size_t size);

// The file's subtitles into out, which has room for max, in file order. A block is written only once it has
// parsed and there is room; every block left out or too wide is reported with its number in the file.
// Returns how many were kept.
unsigned parse(const char* data, size_t size, Subtitle* out, unsigned max,
               void (*report)(void* context, unsigned number, Problem problem), void* context);

}  // namespace psxsplash::srt
