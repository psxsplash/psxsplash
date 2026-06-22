#pragma once

#include <stdint.h>

#include <psyqo-lua/lua.hh>

namespace psxsplash {

/**
 * Serializes arbitrary Lua values (typically tables) to a compact, tagged
 * binary blob and back, for storing game state on a memory card.
 *
 * The format is integer-only (this psxlua build uses `long` numbers, no FPU),
 * self-describing and length-prefixed. A small header carries a magic, a
 * version and an XOR check so corrupt or foreign data is rejected rather than
 * silently misinterpreted.
 *
 * Supported value types: nil, boolean, number (int32), string, nested tables,
 * and psyqo FixedPoint userdata. Anything else (functions, threads, other
 * userdata) is a hard error. Cyclic or pathologically deep tables are rejected
 * via a depth limit. No operation ever fails silently: every failure reports a
 * human readable reason through `outError`.
 */
class LuaTableSerializer {
  public:
    // Maximum table nesting depth, guarding against cycles and stack overflow.
    static constexpr int c_maxDepth = 16;

    // Size of the fixed header that precedes the serialized value.
    static constexpr uint32_t c_headerSize = 8;

    /**
     * Serializes the Lua value at stack index `idx` into `buffer`.
     *
     * @param outSize Receives the total number of bytes written (header
     * included) on success.
     * @param outError Receives a static error string on failure (may be null
     * on success).
     * @return true on success, false otherwise. The Lua stack is left
     * unchanged.
     */
    static bool serialize(psyqo::Lua& lua, int idx, uint8_t* buffer, uint32_t capacity, uint32_t* outSize,
                          const char** outError);

    /**
     * Deserializes a blob produced by `serialize`, pushing the resulting value
     * onto the Lua stack on success.
     *
     * @param outError Receives a static error string on failure.
     * @return true on success (one value pushed), false otherwise (nothing
     * pushed).
     */
    static bool deserialize(psyqo::Lua& lua, const uint8_t* buffer, uint32_t size, const char** outError);
};

}  // namespace psxsplash
