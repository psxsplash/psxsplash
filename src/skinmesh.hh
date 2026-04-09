#pragma once

#include <stdint.h>

#include <psyqo/matrix.hh>
#include <psyqo/vector.hh>
#include <psyqo/fixed-point.hh>
#include <psyqo/gte-registers.hh>

#include "gameobject.hh"
#include "mesh.hh"

// Forward-declare lua_State to avoid pulling in lua headers
struct lua_State;
#ifndef LUA_NOREF
#define LUA_NOREF (-2)
#endif

namespace psxsplash {

static constexpr uint8_t  SKINMESH_MAX_BONES  = 64;
static constexpr uint8_t  SKINMESH_MAX_CLIPS  = 16;
static constexpr int      MAX_SKINNED_MESHES   = 16;

/// Pre-baked bone matrix: 3×3 rotation (4.12 fp) + translation.
/// Layout matches the GTE rotation register format (9 × int16)
/// plus a 3-component translation (3 × int16).
struct BakedBoneMatrix {
    int16_t r[9];    // row-major: r00,r01,r02, r10,r11,r12, r20,r21,r22
    int16_t t[3];    // translation: tx, ty, tz (model-space scale, 4.12 fp)
};
static_assert(sizeof(BakedBoneMatrix) == 24, "BakedBoneMatrix must be 24 bytes");

/// One animation clip: name, playback settings, and pointer into the scene data buffer.
/// Binary layout (v18): flags(1), fps(1), frameCount(2, little-endian), then frame data.
struct SkinAnimClip {
    const char* name;              // points into splashpack data (null-terminated by loader)
    const BakedBoneMatrix* frames; // points into the scene data buffer
    uint16_t frameCount;           // number of baked frames (no hard cap — user's responsibility)
    uint8_t  flags;                // bit 0 = loops
    uint8_t  fps;                  // baked sampling rate (1-30)
    uint8_t  boneCount;
    uint8_t  _pad[3];
};

/// All clips for one skinned object.
struct SkinAnimSet {
    Tri*         polygons;          // stolen from the GO at init (regular render sees polyCount=0)
    const uint8_t* boneIndices;    // polyCount×3 bone index bytes, points into splashpack data
    uint16_t     polyCount;        // triangle count (moved from GO)
    uint8_t      clipCount;
    uint8_t      boneCount;        // from the skin data (shared across clips)
    uint16_t     gameObjectIndex;  // index into m_gameObjects (still used for transform)
    uint16_t     _pad;
    SkinAnimClip clips[SKINMESH_MAX_CLIPS];
};

/// Per-instance runtime playback state.
struct SkinAnimState {
    SkinAnimSet* animSet;          // points into scene data, never null after load
    uint16_t currentFrame;         // current whole frame index
    uint16_t subFrame;             // 0..4095 (0.12 fixed-point) fraction between currentFrame and next
    uint8_t  currentClip;
    bool     playing;
    bool     loop;                 // runtime loop override (set by Lua Play call)
    uint8_t  _pad;
    int      luaCallbackRef;       // Lua registry reference, LUA_NOREF = none
};

/// Tick the animation state.  dt12 is the frame delta in 0.12 fixed-point
/// (4096 = one 30fps frame).  Framerate-independent.
void SkinMesh_Tick(SkinAnimState* state, lua_State* L, int32_t dt12);

}  // namespace psxsplash
