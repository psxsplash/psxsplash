#pragma once

#include <stdint.h>

namespace psxsplash {

/**
 * Builds the 64-byte Shift-JIS title that the PlayStation BIOS shows for a
 * memory card save. The BIOS renders titles as fullwidth Shift-JIS, so plain
 * ASCII is mapped to its fullwidth equivalents (the encoding commercial games
 * use). At most 32 characters fit; the rest are ignored and the buffer is
 * zero-padded.
 *
 * @param ascii A null-terminated ASCII string.
 * @param out A 64-byte buffer to receive the zero-padded Shift-JIS title.
 */
void asciiToSjisTitle(const char* ascii, uint8_t out[64]);

}  // namespace psxsplash
