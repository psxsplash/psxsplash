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
    if (!state->playing) return;

    const SkinAnimClip& clip = state->animSet->clips[state->currentClip];
    // advance = dt12 * fps / 30
    uint32_t advance = ((uint32_t)dt12 * (uint32_t)clip.fps) / 30u;

    uint32_t accum = (uint32_t)state->subFrame + advance;

    uint16_t wholeFrames = (uint16_t)(accum >> 12);
    state->subFrame = (uint16_t)(accum & 0xFFF);
    state->currentFrame += wholeFrames;

    if (state->currentFrame >= clip.frameCount) {
        if (state->loop || (clip.flags & 0x01)) {
            // Looping — wrap
            state->currentFrame = state->currentFrame % clip.frameCount;
        } else {
            // Stop at last frame
            state->currentFrame = clip.frameCount - 1;
            state->subFrame = 0;
            state->playing = false;

            // Fire Lua callback if registered
            if (state->luaCallbackRef != LUA_NOREF && L) {
                lua_rawgeti(L, LUA_REGISTRYINDEX, state->luaCallbackRef);
                lua_pcall(L, 0, 0, 0);
                luaL_unref(L, LUA_REGISTRYINDEX, state->luaCallbackRef);
                state->luaCallbackRef = LUA_NOREF;
            }
            return;
        }
    }
}

}  // namespace psxsplash
