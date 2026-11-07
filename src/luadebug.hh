#pragma once

// Lua breakpoints for an editor driving a PCdrv build (SplashEdit's Play).
//
// When "luabp.txt" exists in the PCdrv directory at the first scene load,
// the session is on: the engine re-reads the file every LUA_DEBUG_PERIOD
// frames and, while it lists any breakpoint, runs the VM with a line hook.
// Without the file nothing is installed and nothing is polled, so a game
// that is not being debugged runs the VM exactly as before. LOADER=cdrom
// builds have no PCdrv and compile this to nothing.
//
// On a hit (or the next line after a step) the engine prints, on the TTY the
// editor reads:
//     psxsplash: lua break <id> <line> <chunk>
//     psxsplash: lua local <name> <type> <value>      (the stopped function's locals)
//     psxsplash: lua upvalue <name> <type> <value>    (file-level locals it uses)
//     psxsplash: lua global <name> <type> <value>     (the script's globals)
//     psxsplash: lua break end <id>
// then waits, polling "luabp.cmd" over PCdrv, for "<id> continue" or
// "<id> step" (luadebugmath.hh), and prints "psxsplash: lua resume <id>
// <continue|step>". The wait spins on the hblank counter between polls and
// runs no psyqo callbacks, so nothing re-enters Lua while it is stopped; the
// interrupts keep running, so the emulator (or a serial PCdrv host) keeps
// answering.

#include <stdint.h>

struct lua_State;
struct lua_Debug;

namespace psxsplash {

class LuaDebugger {
  public:
    static LuaDebugger& Get();
    // After each fresh VM (scene load, hot reload), before any script runs.
    // The first call decides whether the session is on.
    void attach(lua_State* L);
    // Once per frame.
    void poll();
    bool active() const { return m_session; }

  private:
    LuaDebugger() = default;
    bool readBreakpoints();
    void updateHook();
    static void hook(lua_State* L, lua_Debug* ar);
    void stop(lua_State* L, lua_Debug* ar);

    lua_State* m_L = nullptr;
    bool m_checked = false;
    bool m_session = false;
    bool m_stepping = false;
    bool m_hooked = false;
    bool m_inStop = false;
    uint8_t m_countdown = 0;
    uint32_t m_generation = 0;
    uint32_t m_stopId = 0;
    int m_count = 0;
};

}  // namespace psxsplash
