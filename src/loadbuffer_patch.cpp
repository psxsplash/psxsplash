// Linker --wrap intercept for luaL_loadbufferx.
//
// psyqo-lua's FixedPoint metatable setup (at the pinned nugget commit)
// loads Lua SOURCE text via loadBuffer. With NOPARSER, the parser stub
// rejects it with "parser not loaded". This wrapper intercepts that
// specific call and redirects it to pre-compiled bytecode.
//
// All other loadBuffer calls pass through to the real implementation.

#include "fixedpoint_patch.h"

extern "C" {

// The real luaL_loadbufferx provided by liblua
int __real_luaL_loadbufferx(void* L, const char* buff, unsigned int size,
                            const char* name, const char* mode);

int __wrap_luaL_loadbufferx(void* L, const char* buff, unsigned int size,
                            const char* name, const char* mode) {
    // Check if this is the fixedpoint script load from psyqo-lua.
    // The name is "buffer:fixedPointScript" in the pinned nugget version.
    if (name) {
        // Compare the first few chars to identify the fixedpoint load
        const char* expected = "buffer:fixedPointScript";
        const char* a = name;
        const char* b = expected;
        bool match = true;
        while (*b) {
            if (*a != *b) { match = false; break; }
            a++;
            b++;
        }
        if (match && *a == '\0') {
            // Redirect to pre-compiled bytecode
            return __real_luaL_loadbufferx(
                L,
                reinterpret_cast<const char*>(FIXEDPOINT_PATCHED_BYTECODE),
                sizeof(FIXEDPOINT_PATCHED_BYTECODE),
                "bytecode:fixedPointScript",
                mode);
        }
    }

    // All other calls pass through unchanged
    return __real_luaL_loadbufferx(L, buff, size, name, mode);
}

}  // extern "C"
