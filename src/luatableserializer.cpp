#include "luatableserializer.hh"

#include <psyqo/fixed-point.hh>

namespace psxsplash {

namespace {

// Value type tags. Stored as a single leading byte before each value.
enum Tag : uint8_t {
    TAG_NIL = 0,
    TAG_FALSE = 1,
    TAG_TRUE = 2,
    TAG_NUMBER = 3,      // + int32 little-endian
    TAG_STRING = 4,      // + uint16 length + bytes
    TAG_TABLE = 5,       // + (key value)* + TAG_END
    TAG_FIXEDPOINT = 6,  // + int32 little-endian raw value
    TAG_END = 7,         // table terminator
};

// Header layout: 'S','S', version, xor-of-payload, uint32 payload length.
constexpr uint8_t c_magic0 = 'S';
constexpr uint8_t c_magic1 = 'S';
constexpr uint8_t c_version = 1;

// A bounds-checked sink. Once it overflows it stops writing and latches the
// overflow flag, so the caller learns the data did not fit instead of silently
// truncating.
struct Writer {
    uint8_t* buffer;
    uint32_t capacity;
    uint32_t pos = 0;
    bool overflow = false;

    void u8(uint8_t v) {
        if (pos >= capacity) {
            overflow = true;
            return;
        }
        buffer[pos++] = v;
    }
    void u16(uint16_t v) {
        u8(static_cast<uint8_t>(v));
        u8(static_cast<uint8_t>(v >> 8));
    }
    void u32(uint32_t v) {
        u8(static_cast<uint8_t>(v));
        u8(static_cast<uint8_t>(v >> 8));
        u8(static_cast<uint8_t>(v >> 16));
        u8(static_cast<uint8_t>(v >> 24));
    }
    void bytes(const void* p, uint32_t n) {
        const uint8_t* src = reinterpret_cast<const uint8_t*>(p);
        for (uint32_t i = 0; i < n; i++) u8(src[i]);
    }
};

// A bounds-checked source. Reads past the end latch the error flag and return
// zeros, so a corrupt blob can never run off the end of the buffer.
struct Reader {
    const uint8_t* buffer;
    uint32_t size;
    uint32_t pos = 0;
    bool error = false;

    uint8_t peek() {
        if (pos >= size) {
            error = true;
            return 0;
        }
        return buffer[pos];
    }
    uint8_t u8() {
        if (pos >= size) {
            error = true;
            return 0;
        }
        return buffer[pos++];
    }
    uint16_t u16() {
        uint16_t lo = u8();
        uint16_t hi = u8();
        return static_cast<uint16_t>(lo | (hi << 8));
    }
    uint32_t u32() {
        uint32_t b0 = u8();
        uint32_t b1 = u8();
        uint32_t b2 = u8();
        uint32_t b3 = u8();
        return b0 | (b1 << 8) | (b2 << 16) | (b3 << 24);
    }
    const uint8_t* take(uint32_t n) {
        if (pos + n > size) {
            error = true;
            return nullptr;
        }
        const uint8_t* p = buffer + pos;
        pos += n;
        return p;
    }
};

bool writeValue(psyqo::Lua& lua, int idx, Writer& w, int depth, const char** outError);

bool writeTable(psyqo::Lua& lua, int idx, Writer& w, int depth, const char** outError) {
    if (depth >= LuaTableSerializer::c_maxDepth) {
        *outError = "table nesting too deep (cycle?)";
        return false;
    }
    w.u8(TAG_TABLE);

    int tableIdx = lua.getabsolute(idx);
    lua.checkStack(4, "memcard serialize");
    lua.push();  // initial nil key for lua_next
    while (lua.next(tableIdx) != 0) {
        // key is at top-1, value is at top. Serialize the key first. We must
        // never run lua_tolstring on a numeric key (it would corrupt lua_next);
        // writeValue uses toNumber for numbers, so keys are safe.
        int top = lua.getTop();
        if (!writeValue(lua, top - 1, w, depth + 1, outError)) {
            lua.pop(2);  // drop value + key
            return false;
        }
        if (!writeValue(lua, top, w, depth + 1, outError)) {
            lua.pop(2);
            return false;
        }
        lua.pop(1);  // drop value, keep key for the next iteration
    }
    w.u8(TAG_END);
    return true;
}

bool writeValue(psyqo::Lua& lua, int idx, Writer& w, int depth, const char** outError) {
    // FixedPoint is userdata, so test it before the generic type switch.
    if (lua.isFixedPoint(idx)) {
        w.u8(TAG_FIXEDPOINT);
        w.u32(static_cast<uint32_t>(lua.toFixedPoint(idx).raw()));
        return true;
    }

    switch (lua.type(idx)) {
        case LUA_TNIL:
            w.u8(TAG_NIL);
            return true;
        case LUA_TBOOLEAN:
            w.u8(lua.toBoolean(idx) ? TAG_TRUE : TAG_FALSE);
            return true;
        case LUA_TNUMBER: {
            w.u8(TAG_NUMBER);
            w.u32(static_cast<uint32_t>(static_cast<int32_t>(lua.toNumber(idx))));
            return true;
        }
        case LUA_TSTRING: {
            size_t len = 0;
            const char* s = lua.toString(idx, &len);
            if (len > 0xffff) {
                *outError = "string too long to serialize";
                return false;
            }
            w.u8(TAG_STRING);
            w.u16(static_cast<uint16_t>(len));
            w.bytes(s, static_cast<uint32_t>(len));
            return true;
        }
        case LUA_TTABLE:
            return writeTable(lua, idx, w, depth, outError);
        default:
            *outError = "unsupported value type in table (functions/userdata)";
            return false;
    }
}

bool readValue(psyqo::Lua& lua, Reader& r, int depth, const char** outError);

bool readTable(psyqo::Lua& lua, Reader& r, int depth, const char** outError) {
    if (depth >= LuaTableSerializer::c_maxDepth) {
        *outError = "table nesting too deep";
        return false;
    }
    lua.checkStack(4, "memcard deserialize");
    lua.newTable();
    int tableIdx = lua.getTop();
    while (true) {
        if (r.error) {
            *outError = "truncated save data";
            return false;
        }
        if (r.peek() == TAG_END) {
            r.u8();
            break;
        }
        if (!readValue(lua, r, depth + 1, outError)) return false;  // key
        if (!readValue(lua, r, depth + 1, outError)) return false;  // value
        lua.setTable(tableIdx);                                     // t[key] = value
    }
    return true;
}

bool readValue(psyqo::Lua& lua, Reader& r, int depth, const char** outError) {
    uint8_t tag = r.u8();
    if (r.error) {
        *outError = "truncated save data";
        return false;
    }
    switch (tag) {
        case TAG_NIL:
            lua.push();
            return true;
        case TAG_FALSE:
            lua.push(false);
            return true;
        case TAG_TRUE:
            lua.push(true);
            return true;
        case TAG_NUMBER:
            lua.pushNumber(static_cast<lua_Number>(static_cast<int32_t>(r.u32())));
            return true;
        case TAG_FIXEDPOINT:
            lua.push(psyqo::FixedPoint<>(static_cast<int32_t>(r.u32()), psyqo::FixedPoint<>::RAW));
            return true;
        case TAG_STRING: {
            uint16_t len = r.u16();
            const uint8_t* p = r.take(len);
            if (r.error || (len != 0 && !p)) {
                *outError = "truncated string in save data";
                return false;
            }
            lua.push(reinterpret_cast<const char*>(p), len);
            return true;
        }
        case TAG_TABLE:
            return readTable(lua, r, depth, outError);
        default:
            *outError = "corrupt save data (bad tag)";
            return false;
    }
}

}  // namespace

bool LuaTableSerializer::serialize(psyqo::Lua& lua, int idx, uint8_t* buffer, uint32_t capacity, uint32_t* outSize,
                                   const char** outError) {
    const char* dummy = nullptr;
    if (!outError) outError = &dummy;
    *outError = nullptr;
    if (outSize) *outSize = 0;

    if (capacity < c_headerSize) {
        *outError = "save buffer too small";
        return false;
    }

    int absIdx = lua.getabsolute(idx);

    Writer w{buffer, capacity};
    w.pos = c_headerSize;  // reserve room for the header
    if (!writeValue(lua, absIdx, w, 0, outError)) return false;
    if (w.overflow) {
        *outError = "serialized data too large";
        return false;
    }

    uint32_t payloadLen = w.pos - c_headerSize;
    uint8_t check = 0;
    for (uint32_t i = 0; i < payloadLen; i++) check ^= buffer[c_headerSize + i];

    buffer[0] = c_magic0;
    buffer[1] = c_magic1;
    buffer[2] = c_version;
    buffer[3] = check;
    buffer[4] = static_cast<uint8_t>(payloadLen);
    buffer[5] = static_cast<uint8_t>(payloadLen >> 8);
    buffer[6] = static_cast<uint8_t>(payloadLen >> 16);
    buffer[7] = static_cast<uint8_t>(payloadLen >> 24);

    if (outSize) *outSize = w.pos;
    return true;
}

bool LuaTableSerializer::deserialize(psyqo::Lua& lua, const uint8_t* buffer, uint32_t size, const char** outError) {
    const char* dummy = nullptr;
    if (!outError) outError = &dummy;
    *outError = nullptr;

    if (size < c_headerSize) {
        *outError = "save data too small";
        return false;
    }
    if (buffer[0] != c_magic0 || buffer[1] != c_magic1) {
        *outError = "not a valid save (bad magic)";
        return false;
    }
    if (buffer[2] != c_version) {
        *outError = "unsupported save version";
        return false;
    }

    uint32_t payloadLen = static_cast<uint32_t>(buffer[4]) | (static_cast<uint32_t>(buffer[5]) << 8) |
                          (static_cast<uint32_t>(buffer[6]) << 16) | (static_cast<uint32_t>(buffer[7]) << 24);
    if (payloadLen > size - c_headerSize) {
        *outError = "corrupt save data (length overrun)";
        return false;
    }

    uint8_t check = 0;
    for (uint32_t i = 0; i < payloadLen; i++) check ^= buffer[c_headerSize + i];
    if (check != buffer[3]) {
        *outError = "corrupt save data (checksum mismatch)";
        return false;
    }

    Reader r{buffer + c_headerSize, payloadLen};
    if (!readValue(lua, r, 0, outError)) return false;
    return true;
}

}  // namespace psxsplash
