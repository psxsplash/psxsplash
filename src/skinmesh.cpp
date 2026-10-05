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
            // Looping - wrap
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

bool SkinMesh_CurrentFrames(const SkinAnimSet& set, const SkinAnimState& state,
                            const BakedBoneMatrix** a, const BakedBoneMatrix** b, uint16_t* blend) {
    if (set.clipCount == 0) return false;
    uint8_t clipIdx = state.currentClip;
    if (clipIdx >= set.clipCount) clipIdx = 0;
    const SkinAnimClip& clip = set.clips[clipIdx];
    if (!clip.frames || clip.frameCount == 0) return false;

    uint16_t frame = state.currentFrame;
    if (frame >= clip.frameCount) frame = clip.frameCount - 1;
    *a = &clip.frames[(uint32_t)frame * set.boneCount];
    *b = nullptr;
    *blend = 0;

    uint16_t sf = state.subFrame;
    if (sf > 0 && frame + 1 < clip.frameCount) {
        *b = &clip.frames[(uint32_t)(frame + 1) * set.boneCount];
        *blend = sf;
    } else if (sf > 0 && (state.loop || (clip.flags & 0x01)) && clip.frameCount > 1) {
        // Looping: blend the last frame into the first.
        *b = &clip.frames[0];
        *blend = sf;
    }
    return true;
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
