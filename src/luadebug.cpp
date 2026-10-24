#include "luadebug.hh"

#if !defined(LOADER_CDROM)

#include <psyqo-lua/lua.hh>
#include <psyqo/xprintf.h>

#include "common/hardware/counters.h"
#include "common/syscalls/syscalls.h"
#include "luadebugmath.hh"
#include "pcdrv_handler.hh"

namespace {

using namespace psxsplash;
using namespace psxsplash::luadebug;

constexpr uint8_t LUA_DEBUG_PERIOD = 15;  // frames between reads of luabp.txt
constexpr uint32_t kPollHblanks = 1600;   // about 0.1 s between reads of luabp.cmd
constexpr int kMaxLocals = 32, kMaxGlobals = 40;
constexpr int kValueCap = 96;

// Allocated when a session turns on, so a build that is not being debugged
// keeps the RAM.
constexpr int kFileCap = kMaxBreakpoints * (kMaxChunk + 8) + 16;
Breakpoint* s_bps = nullptr;
char* s_file = nullptr;

// Reads `name` from the PCdrv directory into `buf`; -1 when it does not exist.
int readSmall(const char* name, char* buf, int cap) {
    int fd = pcdrv_open(name, 0, 0);
    if (fd < 0) return -1;
    int n = pcdrv_read(fd, buf, cap);
    pcdrv_close(fd);
    return n < 0 ? 0 : n;
}

// Spins for `n` hblanks on root counter 1, which psyqo leaves counting them.
// Interrupts keep being serviced; no psyqo callback runs.
void waitHblanks(uint32_t n) {
    uint16_t last = COUNTERS[1].value;
    uint32_t passed = 0;
    while (passed < n) {
        uint16_t now = COUNTERS[1].value;
        passed += uint16_t(now - last);
        last = now;
    }
}

// One scalar value, as printed: numbers in decimal, strings quoted.
int scalar(lua_State* L, int idx, char* out, int cap) {
    switch (lua_type(L, idx)) {
        case LUA_TNIL:
            return snprintf(out, cap, "nil");
        case LUA_TBOOLEAN:
            return snprintf(out, cap, "%s", lua_toboolean(L, idx) ? "true" : "false");
        case LUA_TNUMBER:
            return snprintf(out, cap, "%d", (int)lua_tonumber(L, idx));
        case LUA_TSTRING: {
            size_t len = 0;
            const char* s = lua_tolstring(L, idx, &len);
            if (cap < 10) return 0;
            out[0] = '"';
            int n = escape(s, int(len), out + 1, cap - 2);
            out[n + 1] = '"';
            out[n + 2] = 0;
            return n + 2;
        }
        default:
            return snprintf(out, cap, "%s", lua_typename(L, lua_type(L, idx)));
    }
}

// A table's first fields: {x=1, name="a", 3 more}. Only string and number
// keys are shown; values one level deep.
void tableText(lua_State* L, int idx, char* out, int cap) {
    idx = lua_absindex(L, idx);
    int o = snprintf(out, cap, "{");
    int shown = 0, more = 0;
    lua_pushnil(L);
    while (lua_next(L, idx)) {
        const int kt = lua_type(L, -2);
        if (shown >= 6 || o > cap - 56 || (kt != LUA_TSTRING && kt != LUA_TNUMBER)) {
            more++;
            lua_pop(L, 1);
            continue;
        }
        if (shown) o += snprintf(out + o, cap - o, ", ");
        if (kt == LUA_TSTRING) {
            size_t kl = 0;
            const char* k = lua_tolstring(L, -2, &kl);
            o += escape(k, int(kl), out + o, cap - o > 20 ? 20 : cap - o);
        } else {
            o += snprintf(out + o, cap - o, "[%d]", (int)lua_tonumber(L, -2));
        }
        o += snprintf(out + o, cap - o, "=");
        if (lua_type(L, -1) == LUA_TTABLE) o += snprintf(out + o, cap - o, "{...}");
        else o += scalar(L, -1, out + o, cap - o > 24 ? 24 : cap - o);
        if (o > cap - 1) o = cap - 1;
        shown++;
        lua_pop(L, 1);
    }
    if (o > cap - 16) o = cap - 16;
    if (more) o += snprintf(out + o, cap - o, "%s%d more", shown ? ", " : "", more);
    snprintf(out + o, cap - o, "}");
}

void valueText(lua_State* L, int idx, char* out, int cap) {
    if (lua_type(L, idx) == LUA_TTABLE) tableText(L, idx, out, cap);
    else scalar(L, idx, out, cap);
}

// A table of the engine's API (Entity, Debug, Input, ...) rather than game
// data: one of its first 64 fields is a C function.
bool isApiTable(lua_State* L, int idx) {
    idx = lua_absindex(L, idx);
    int seen = 0;
    lua_pushnil(L);
    while (lua_next(L, idx)) {
        const bool c = lua_iscfunction(L, -1);
        lua_pop(L, 1);
        if (c || ++seen >= 64) {
            lua_pop(L, 1);
            return c;
        }
    }
    return false;
}

void printVar(const char* kind, const char* prefix, const char* name, lua_State* L, int idx) {
    char value[kValueCap];
    valueText(L, idx, value, sizeof(value));
    ramsyscall_printf("psxsplash: lua %s %s%s %s %s\n", kind, prefix, name, lua_typename(L, lua_type(L, idx)), value);
}

// The globals in table `idx`: no functions and no API tables. Returns how
// many were printed, at most `room`.
int printGlobals(lua_State* L, int idx, const char* prefix, int room, int skipIdx) {
    idx = lua_absindex(L, idx);
    if (skipIdx) skipIdx = lua_absindex(L, skipIdx);
    int printed = 0;
    lua_pushnil(L);
    while (lua_next(L, idx)) {
        const int vt = lua_type(L, -1);
        bool show = lua_type(L, -2) == LUA_TSTRING && vt != LUA_TFUNCTION && printed < room;
        if (show && vt == LUA_TTABLE) show = !isApiTable(L, -1) && !lua_rawequal(L, -1, idx);
        // _G's own names (_VERSION, _G) are the VM's.
        if (show && skipIdx) show = lua_tostring(L, -2)[0] != '_';
        // A name the script's own table already has hides the shared one.
        if (show && skipIdx) {
            lua_pushvalue(L, -2);
            lua_rawget(L, skipIdx);
            show = lua_isnil(L, -1);
            lua_pop(L, 1);
        }
        if (show) {
            size_t kl = 0;
            const char* k = lua_tolstring(L, -2, &kl);
            char name[40];
            escape(k, int(kl), name, sizeof(name));
            printVar("global", prefix, name, L, -1);
            printed++;
        }
        lua_pop(L, 1);
    }
    return printed;
}

}  // namespace

psxsplash::LuaDebugger& psxsplash::LuaDebugger::Get() {
    static LuaDebugger instance;
    return instance;
}

bool psxsplash::LuaDebugger::readBreakpoints() {
    if (!s_file) return false;
    int n = readSmall("luabp.txt", s_file, kFileCap - 1);
    if (n < 0) return false;
    uint32_t gen = 0;
    int count = parseBreakpoints(s_file, n, s_bps, kMaxBreakpoints, &gen);
    if (gen == m_generation && m_checked && m_count == count) return false;
    m_generation = gen;
    m_count = count;
    return true;
}

void psxsplash::LuaDebugger::updateHook() {
    const bool want = m_L && (m_count > 0 || m_stepping);
    if (want == m_hooked) return;
    lua_sethook(m_L, want ? hook : nullptr, want ? LUA_MASKLINE : 0, 0);
    m_hooked = want;
}

void psxsplash::LuaDebugger::attach(lua_State* L) {
    m_L = L;
    m_hooked = false;  // a fresh VM has no hook
    if (!m_checked) {
        char probe[4];
        m_session = readSmall("luabp.txt", probe, sizeof(probe)) >= 0;
        if (m_session) {
            s_bps = new Breakpoint[kMaxBreakpoints];
            s_file = new char[kFileCap];
            m_count = -1;  // force the first read to count
            readBreakpoints();
            ramsyscall_printf("psxsplash: lua debugger on, %d breakpoints\n", m_count);
        }
        m_checked = true;
    }
    if (m_session) updateHook();
}

void psxsplash::LuaDebugger::poll() {
    if (!m_session || m_inStop) return;
    if (m_countdown > 0) {
        m_countdown--;
        return;
    }
    m_countdown = LUA_DEBUG_PERIOD;
    if (readBreakpoints()) updateHook();
}

void psxsplash::LuaDebugger::hook(lua_State* L, lua_Debug* ar) {
    LuaDebugger& d = Get();
    if (ar->event != LUA_HOOKLINE || d.m_inStop) return;
    if (!d.m_stepping && !lineHasBreakpoint(s_bps, d.m_count, ar->currentline)) return;
    if (!lua_getinfo(L, "S", ar)) return;
    if (d.m_stepping || hits(s_bps, d.m_count, ar->source, ar->currentline)) d.stop(L, ar);
}

void psxsplash::LuaDebugger::stop(lua_State* L, lua_Debug* ar) {
    if (!lua_checkstack(L, 16)) return;
    m_inStop = true;
    const uint32_t id = ++m_stopId;
    const int top = lua_gettop(L);
    ramsyscall_printf("psxsplash: lua break %d %d %s\n", (int)id, ar->currentline, ar->source);

    // Locals of the stopped function; "(*temporary)" and the like are the VM's own.
    int shown = 0;
    for (int i = 1; shown < kMaxLocals; i++) {
        const char* name = lua_getlocal(L, ar, i);
        if (!name) break;
        if (name[0] != '(') {
            printVar("local", "", name, L, -1);
            shown++;
        }
        lua_pop(L, 1);
    }

    // Upvalues: the script's file-level locals the function uses. Then the
    // globals: the script's own environment (each script runs in a table that
    // falls back to _G), then what _G adds.
    int room = kMaxGlobals;
    if (lua_getinfo(L, "f", ar)) {
        const int fn = lua_gettop(L);
        int env = 0;
        for (int i = 1;; i++) {
            const char* up = lua_getupvalue(L, fn, i);
            if (!up) break;
            const bool isEnv = up[0] == '_' && up[1] == 'E' && up[2] == 'N' && up[3] == 'V' && up[4] == 0;
            if (isEnv && !env && lua_istable(L, -1)) env = i;
            else if (!isEnv && up[0] && lua_type(L, -1) != LUA_TFUNCTION && shown < kMaxLocals) {
                printVar("upvalue", "", up, L, -1);
                shown++;
            }
            lua_pop(L, 1);
        }
        if (env) lua_getupvalue(L, fn, env);
        else lua_pushglobaltable(L);
        const int envIdx = lua_gettop(L);
        room -= printGlobals(L, envIdx, "", room, 0);
        lua_pushglobaltable(L);
        if (room > 0 && !lua_rawequal(L, -1, envIdx)) printGlobals(L, -1, "_G.", room, envIdx);
    }
    lua_settop(L, top);
    ramsyscall_printf("psxsplash: lua break end %d\n", (int)id);

    // Wait for the editor.
    char cmd[64];
    Command c = Command::None;
    while (c == Command::None) {
        waitHblanks(kPollHblanks);
        int n = readSmall("luabp.cmd", cmd, sizeof(cmd) - 1);
        if (n > 0) c = parseCommand(cmd, n, id);
    }
    m_stepping = c == Command::Step;
    ramsyscall_printf("psxsplash: lua resume %d %s\n", (int)id, m_stepping ? "step" : "continue");
    // Breakpoints changed while stopped take effect now.
    readBreakpoints();
    updateHook();
    m_countdown = LUA_DEBUG_PERIOD;
    m_inStop = false;
}

#else  // LOADER_CDROM: no PCdrv, no debugger.

psxsplash::LuaDebugger& psxsplash::LuaDebugger::Get() {
    static LuaDebugger instance;
    return instance;
}
void psxsplash::LuaDebugger::attach(lua_State*) {}
void psxsplash::LuaDebugger::poll() {}

#endif
