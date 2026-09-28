#pragma once

/**
 * Host-build compatibility shim. Test builds only — engine sources are never
 * modified to accommodate the host compiler.
 *
 * The engine targets GCC/Clang for MIPS and calls the __builtin_ memory
 * intrinsics directly, because those lower to inline code without pulling in
 * libc under -nostdlib. MSVC has no such spelling, so map them onto the
 * standard functions (which MSVC treats as intrinsics anyway). GCC and Clang
 * need nothing here.
 */

#if defined(_MSC_VER) && !defined(__clang__)
#include <string.h>
#define __builtin_memcpy memcpy
#define __builtin_memcmp memcmp
#endif
