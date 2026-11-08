#include "skinmesh.hh"

#include <psyqo/gte-kernels.hh>
#include <psyqo/gte-registers.hh>
#include <psyqo/kernel.hh>
#include <psyqo/soft-math.hh>

#include "gtemath.hh"

extern "C" {
#include <lua.h>
#include <lauxlib.h>
}

using namespace psyqo::GTE;

namespace psxsplash {

// ============================================================================
// Animation tick
// ============================================================================

void SkinMesh_Tick(SkinAnimState* state, lua_State* L, int32_t dt12) {
    // A crossfade advances even when the current clip has stopped, so a fade
    // into a short one-shot still completes.
    SkinPose_TickFade(state, dt12);

    if (!state->playing) return;

    const SkinAnimClip& clip = state->animSet->clips[state->currentClip];
    if (SkinPose_Advance(clip, state->loop, &state->currentFrame, &state->subFrame, dt12)) {
        // Stopped at the last frame
        state->playing = false;

        // Fire Lua callback if registered
        if (state->luaCallbackRef != LUA_NOREF && L) {
            lua_rawgeti(L, LUA_REGISTRYINDEX, state->luaCallbackRef);
            lua_pcall(L, 0, 0, 0);
            luaL_unref(L, LUA_REGISTRYINDEX, state->luaCallbackRef);
            state->luaCallbackRef = LUA_NOREF;
        }
    }
}

int SkinMesh_FindBone(const SkinAnimSet& set, const char* name) {
    const uint8_t* p = set.boneNames;
    if (!p) return -1;
    for (int bi = 0; bi < set.boneCount; bi++) {
        uint8_t len = *p++;
        const char* n = reinterpret_cast<const char*>(p);
        int i = 0;
        while (i < len && name[i] == n[i]) i++;
        if (i == len && name[i] == '\0') return bi;
        p += len + 1;
    }
    return -1;
}

}  // namespace psxsplash
