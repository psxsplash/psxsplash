#pragma once

#include <stdint.h>

// The skinned-animation data model and the pose arithmetic over it, free of
// psyqo, Lua and every global, the same split as spritemath.hh. That is what
// lets tests/host/test_skinpose.cpp build this exact code natively.
//
// Everything that needs the hardware or Lua lives in skinmesh.hh, which
// includes this.

namespace psxsplash {

class Tri;  // mesh.hh; only held by pointer here

static constexpr uint8_t  SKINMESH_MAX_BONES  = 64;
static constexpr uint8_t  SKINMESH_MAX_CLIPS  = 16;
static constexpr int      MAX_SKINNED_MESHES   = 16;

/// Pre-baked bone matrix: 3x3 rotation (4.12 fp) + translation.
/// Layout matches the GTE rotation register format (9 x int16)
/// plus a 3-component translation (3 x int16).
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
    uint16_t frameCount;           // number of baked frames (no hard cap - user's responsibility)
    uint8_t  flags;                // bit 0 = loops
    uint8_t  fps;                  // baked sampling rate (1-30)
    uint8_t  boneCount;
    uint8_t  _pad[3];
};

/// All clips for one skinned object.
struct SkinAnimSet {
    Tri*         polygons;          // stolen from the GO at init (regular render sees polyCount=0)
    const uint8_t* boneIndices;    // polyCountx3 bone index bytes, points into splashpack data
    // v26+, null on older packs. Bind-pose joint positions, boneCount x {x, y, z}
    // in the vertices' space and units (4.12 GTE units, Y down), so a baked bone
    // matrix maps a joint's bind position to its posed position.
    const int16_t* bindPositions;
    // v26+, null on older packs. boneCount records of {u8 len, name, u8 0}.
    const uint8_t* boneNames;
    uint16_t     polyCount;        // triangle count (moved from GO)
    uint8_t      clipCount;
    uint8_t      boneCount;        // from the skin data (shared across clips)
    uint16_t     gameObjectIndex;  // index into m_gameObjects (still used for transform)
    uint16_t     _pad;
    SkinAnimClip clips[SKINMESH_MAX_CLIPS];
};

/// prevFlags bits.
static constexpr uint8_t SKINPOSE_PREV_LOOP    = 0x01;  // previous clip wraps
static constexpr uint8_t SKINPOSE_PREV_PLAYING = 0x02;  // previous clip still advances

/// Per-instance runtime playback state.
struct SkinAnimState {
    SkinAnimSet* animSet;          // points into scene data, never null after load
    uint16_t currentFrame;         // current whole frame index
    uint16_t subFrame;             // 0..4095 (0.12 fixed-point) fraction between currentFrame and next
    uint8_t  currentClip;
    bool     playing;
    bool     loop;                 // runtime loop override (set by Lua Play call)
    uint8_t  prevClip;             // crossfade source clip (valid while fadeLength > 0)
    int      luaCallbackRef;       // Lua registry reference, LUA_NOREF = none
    // Crossfade from the previous clip into the current one. fadeLength 0 = no
    // fade (the pose is the current clip alone). The previous clip keeps
    // playing at its own fps during the fade and never fires onComplete.
    uint16_t prevFrame;
    uint16_t prevSubFrame;
    uint32_t fadePos;              // 4.12 frames elapsed, 0..fadeLength << 12
    uint16_t fadeLength;           // fade duration in 30 fps frames
    uint8_t  prevFlags;            // SKINPOSE_PREV_*
    uint8_t  _pad;
};

/// The two baked frames the renderer blends this frame, and the blend weight
/// (0..4095). b is null when a single frame is drawn. Returns false when the
/// object has nothing to draw. Current clip only; ignores any crossfade.
bool SkinMesh_CurrentFrames(const SkinAnimSet& set, const SkinAnimState& state,
                            const BakedBoneMatrix** a, const BakedBoneMatrix** b, uint16_t* blend);

/// Frame selection for any clip at (frame, subFrame). loop is the runtime loop
/// override; the clip's own loop flag is honoured as well.
bool SkinPose_ClipFrames(const SkinAnimSet& set, uint8_t clipIdx, uint16_t frame, uint16_t subFrame,
                         bool loop, const BakedBoneMatrix** a, const BakedBoneMatrix** b, uint16_t* blend);

/// Advance a clip position by dt12 (4096 = one 30 fps frame) at the clip's fps.
/// Wraps when looping. Otherwise, on reaching the end, holds the last frame
/// (subFrame 0) and returns true.
bool SkinPose_Advance(const SkinAnimClip& clip, bool loop, uint16_t* frame, uint16_t* subFrame, int32_t dt12);

/// True while a crossfade is blending in a previous clip.
inline bool SkinPose_Fading(const SkinAnimState& state) { return state.fadeLength != 0; }

/// The crossfade weight, 0 (all previous clip) .. 4096 (all current clip).
uint16_t SkinPose_FadeWeight(const SkinAnimState& state);

/// One bone of the crossfade, weight w (0 = prev, 4096 = cur). The rotation
/// is lerped and re-orthonormalised (Gram-Schmidt on the bone's axes, R's
/// columns), so the bone keeps its length; near-opposite poses take the
/// nearer pose's rotation. With bind (the bone's bind joint, 3 x int16) the
/// joint R*j+t moves on a straight line; without it t is lerped. Both lerps
/// round to nearest. The ends (w 0 / 4096) return the poses unchanged.
void SkinPose_FadeBone(const BakedBoneMatrix& prev, const BakedBoneMatrix& cur, int32_t w,
                       const int16_t* bind, BakedBoneMatrix* out);

/// The drawn pose: writes boneCount (capped at SKINMESH_MAX_BONES) matrices,
/// the current clip's frame lerp, blended from the previous clip's while a
/// crossfade runs. Returns false when there is nothing to draw.
bool SkinPose_Compute(const SkinAnimSet& set, const SkinAnimState& state, BakedBoneMatrix* out);

/// One bone of the same pose, for queries. Returns false when there is nothing
/// to draw or bone is out of range.
bool SkinPose_ComputeBone(const SkinAnimSet& set, const SkinAnimState& state, int bone, BakedBoneMatrix* out);

/// Call before switching the current clip. fadeFrames > 0 snapshots the pose
/// source of the current clip as the crossfade's previous clip; 0 cancels any
/// fade (snap). A fade already in flight is dropped: only the old current clip
/// becomes the new previous clip (two clips at most).
void SkinPose_BeginFade(const SkinAnimSet& set, SkinAnimState* state, uint16_t fadeFrames);

/// Advance the previous clip and the fade weight. Ends the fade once
/// fadePos >= fadeLength << 12.
void SkinPose_TickFade(SkinAnimState* state, int32_t dt12);

}  // namespace psxsplash
