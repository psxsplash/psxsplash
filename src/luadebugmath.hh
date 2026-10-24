#pragma once

// The text side of the Lua debugger (luadebug.cpp): the breakpoint list and
// the resume command the editor writes over PCdrv, and the escaping of the
// values the engine prints when it stops. No psyqo and no Lua here, so the
// host tests (tests/host/test_luadebug.cpp) compile it natively.
//
// luabp.txt, written by the editor whenever its breakpoints change:
//     <generation>
//     <line> <chunk>
//     ...
// <line> counts from 1; <chunk> is the script's chunk name as the VM reports
// it (lua_Debug::source): the project path the editor compiled the script
// under, the rest of the line. A line that does not parse is skipped.
//
// luabp.cmd, written by the editor while the game is stopped:
//     <stop id> continue      or      <stop id> step
// A command for another stop than the current one is ignored, so a command
// left over from an earlier stop never resumes a later one.

#include <stdint.h>

namespace psxsplash::luadebug {

static constexpr int kMaxBreakpoints = 32;
static constexpr int kMaxChunk = 47;  // chunk name bytes kept per breakpoint

struct Breakpoint {
    uint16_t line;
    uint8_t len;
    char chunk[kMaxChunk + 1];
};

inline bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\r'; }

// Reads a decimal number at text[*at], skipping leading blanks; false if none.
inline bool readNumber(const char* text, int len, int* at, uint32_t* out) {
    int i = *at;
    while (i < len && isSpace(text[i])) i++;
    if (i >= len || text[i] < '0' || text[i] > '9') return false;
    uint32_t v = 0;
    while (i < len && text[i] >= '0' && text[i] <= '9') {
        v = v * 10 + uint32_t(text[i] - '0');
        i++;
    }
    *at = i;
    *out = v;
    return true;
}

// Parses luabp.txt. Returns how many breakpoints went into `out` (at most
// `max`), and the generation in *generation (0 when the file has none).
inline int parseBreakpoints(const char* text, int len, Breakpoint* out, int max, uint32_t* generation) {
    *generation = 0;
    int at = 0, count = 0;
    // First line: the generation.
    int lineEnd = at;
    while (lineEnd < len && text[lineEnd] != '\n') lineEnd++;
    uint32_t gen = 0;
    int p = at;
    if (readNumber(text, lineEnd, &p, &gen)) *generation = gen;
    at = lineEnd + 1;
    while (at < len && count < max) {
        lineEnd = at;
        while (lineEnd < len && text[lineEnd] != '\n') lineEnd++;
        p = at;
        uint32_t line = 0;
        if (readNumber(text, lineEnd, &p, &line) && line > 0 && line <= 0xffff && p < lineEnd && isSpace(text[p])) {
            while (p < lineEnd && isSpace(text[p])) p++;
            int e = lineEnd;
            while (e > p && isSpace(text[e - 1])) e--;
            const int n = e - p;
            if (n > 0 && n <= kMaxChunk) {
                Breakpoint& b = out[count++];
                b.line = uint16_t(line);
                b.len = uint8_t(n);
                for (int i = 0; i < n; i++) b.chunk[i] = text[p + i];
                b.chunk[n] = 0;
            }
        }
        at = lineEnd + 1;
    }
    return count;
}

enum class Command { None, Continue, Step };

// Parses luabp.cmd: the command if it is for stop `id`, else None.
inline Command parseCommand(const char* text, int len, uint32_t id) {
    int p = 0;
    uint32_t got = 0;
    if (!readNumber(text, len, &p, &got) || got != id) return Command::None;
    while (p < len && isSpace(text[p])) p++;
    auto word = [&](const char* w) {
        int i = 0;
        while (w[i]) {
            if (p + i >= len || text[p + i] != w[i]) return false;
            i++;
        }
        return p + i == len || isSpace(text[p + i]) || text[p + i] == '\n';
    };
    if (word("continue")) return Command::Continue;
    if (word("step")) return Command::Step;
    return Command::None;
}

// Whether a line event at `line` of chunk `source` (NUL-terminated) hits one of `bps`.
inline bool lineHasBreakpoint(const Breakpoint* bps, int count, int line) {
    for (int i = 0; i < count; i++)
        if (bps[i].line == line) return true;
    return false;
}
inline bool hits(const Breakpoint* bps, int count, const char* source, int line) {
    for (int i = 0; i < count; i++) {
        if (bps[i].line != line) continue;
        const char* a = source;
        const char* b = bps[i].chunk;
        while (*a && *a == *b) {
            a++;
            b++;
        }
        if (*a == 0 && *b == 0) return true;
    }
    return false;
}

// Writes `s` (`len` bytes) into `out` (room for `cap` bytes, cap >= 8) as one
// printable line: backslash, newline, tab and other control bytes escaped,
// cut at `cap` - 1 bytes with "..." at the end. Returns the bytes written
// (out is NUL-terminated).
inline int escape(const char* s, int len, char* out, int cap) {
    int o = 0;
    for (int i = 0; i < len; i++) {
        const unsigned char c = (unsigned char)s[i];
        char buf[4];
        int n = 0;
        if (c == '\\') buf[n++] = '\\', buf[n++] = '\\';
        else if (c == '\n') buf[n++] = '\\', buf[n++] = 'n';
        else if (c == '\t') buf[n++] = '\\', buf[n++] = 't';
        else if (c < 32 || c == 127) buf[n++] = '\\', buf[n++] = '?';
        else buf[n++] = char(c);
        if (o + n > cap - 4) {
            out[o++] = '.';
            out[o++] = '.';
            out[o++] = '.';
            out[o] = 0;
            return o;
        }
        for (int k = 0; k < n; k++) out[o++] = buf[k];
    }
    out[o] = 0;
    return o;
}

}  // namespace psxsplash::luadebug
