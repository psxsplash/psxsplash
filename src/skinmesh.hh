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
/*struct SkinAnimSet {
    Tri*         polygons;          // stolen from the GO at init (regular render sees polyCount=0)
    const uint8_t* boneIndices;    // polyCount×3 bone index bytes, points into splashpack data
    uint16_t     polyCount;        // triangle count (moved from GO)
    uint8_t      clipCount;
    uint8_t      boneCount;        // from the skin data (shared across clips)
    uint16_t     gameObjectIndex;  // index into m_gameObjects (still used for transform)
    uint16_t     _pad;
    SkinAnimClip clips[SKINMESH_MAX_CLIPS];
};*/

struct SkinAnimSet {
    Tri* polygons;
    const uint8_t* boneIndices;
    const char* const* boneNames = nullptr; // NEW: not yet populated by the loader —
    // stays nullptr until a future splashpack
    // change adds it. SkinMesh_FindBoneByName
    // already handles null safely.
    uint16_t polyCount;
    uint8_t clipCount;
    uint8_t boneCount;
    uint16_t gameObjectIndex;
    uint16_t _pad;
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

//bool SkinMesh_GetBoneWorldPosition(const SkinAnimSet& animSet, const SkinAnimState& animState,
    //const GameObject& obj, uint8_t boneIndex,
    //psyqo::Vec3& outPos);

/*bool SkinMesh_GetBoneWorldPosition(const SkinAnimSet& animSet, const SkinAnimState& animState,
    const GameObject& obj, uint8_t boneIndex,
    const psyqo::Vec3& localOffset,
    psyqo::Vec3& outPos);*/

    /// Approximates bone `boneIndex`'s BIND-POSE position (mesh-local space) by
    /// averaging the positions of every vertex weighted to that bone in
    /// animSet.polygons/boneIndices. This is an approximation, not the true
    /// joint pivot — it sits wherever the nearby mesh surface sits, which is
    /// usually close enough for attaching props but isn't pixel-exact. Bones
    /// with zero vertices weighted to them have no answer; returns false.
    /// Computed once and cached (see SkinMesh_PrecomputeApproxBindPositions) —
    /// cheap after the first call, not cheap on the first call for a large mesh.
bool SkinMesh_GetApproxBoneBindPosition(const SkinAnimSet& animSet, uint8_t boneIndex,
    psyqo::Vec3& outPos);

/// Eagerly builds the bind-position cache for `animSet` (all bones at once).
/// Call this once per skinned mesh right after scene load (see scenemanager.cpp)
/// so the first SkinMesh_GetBoneWorldPosition() call during gameplay doesn't
/// pay the O(polyCount) scan cost mid-frame. Safe to call more than once —
/// a second call is a no-op if the cache is already built for this animSet.
void SkinMesh_PrecomputeApproxBindPositions(const SkinAnimSet& animSet);

/// Clears the ENTIRE bind-position cache (all skinned meshes). Call this
/// once before repopulating m_skinAnimSets on scene load — the cache keys
/// by SkinAnimSet pointer identity, and those addresses get reused across
/// scene loads with different content, so without this a freshly loaded
/// scene's first bone query could silently return the PREVIOUS scene's
/// cached bind positions.
void SkinMesh_ClearBindPositionCache();

/// Finds a bone's index by name (case-sensitive exact match).
/// Returns -1 if animSet.boneNames is null (still requires a splashpack
/// format addition to populate — not part of this pass) or no bone matches.
int SkinMesh_FindBoneByName(const SkinAnimSet& animSet, const char* name);

/// Computes bone `boneIndex`'s current WORLD-SPACE position for `obj`, using
/// the approximated bind position above. Independent of the camera and of
/// whether Render() has run this frame.
bool SkinMesh_GetBoneWorldPosition(const SkinAnimSet& animSet, const SkinAnimState& animState,
    const GameObject& obj, uint8_t boneIndex,
    psyqo::Vec3& outPos);

}  // namespace psxsplash
