#pragma once

#include <stdint.h>

#include <psyqo/matrix.hh>
#include <psyqo/vector.hh>
#include <psyqo/fixed-point.hh>
#include <psyqo/gte-registers.hh>

#include "gameobject.hh"
#include "mesh.hh"
#include "skinpose.hh"

// Forward-declare lua_State to avoid pulling in lua headers
struct lua_State;
#ifndef LUA_NOREF
#define LUA_NOREF (-2)
#endif

namespace psxsplash {

// BakedBoneMatrix, SkinAnimClip, SkinAnimSet, SkinAnimState and the pose math
// (SkinMesh_CurrentFrames, SkinPose_*) live in skinpose.hh, psyqo-free so the
// host tests can build them.

/// Tick the animation state.  dt12 is the frame delta in 0.12 fixed-point
/// (4096 = one 30fps frame).  Framerate-independent. Also advances a running
/// crossfade (see SkinPose_TickFade).
void SkinMesh_Tick(SkinAnimState* state, lua_State* L, int32_t dt12);

/// Bone index for a joint name, or -1.
int SkinMesh_FindBone(const SkinAnimSet& set, const char* name);

}  // namespace psxsplash
