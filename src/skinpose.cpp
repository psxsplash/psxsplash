#include "skinpose.hh"

namespace psxsplash {

bool SkinPose_ClipFrames(const SkinAnimSet& set, uint8_t clipIdx, uint16_t frame, uint16_t subFrame,
                         bool loop, const BakedBoneMatrix** a, const BakedBoneMatrix** b, uint16_t* blend) {
    if (set.clipCount == 0) return false;
    if (clipIdx >= set.clipCount) clipIdx = 0;
    const SkinAnimClip& clip = set.clips[clipIdx];
    if (!clip.frames || clip.frameCount == 0) return false;

    if (frame >= clip.frameCount) frame = clip.frameCount - 1;
    *a = &clip.frames[(uint32_t)frame * set.boneCount];
    *b = nullptr;
    *blend = 0;

    uint16_t sf = subFrame;
    if (sf > 0 && frame + 1 < clip.frameCount) {
        *b = &clip.frames[(uint32_t)(frame + 1) * set.boneCount];
        *blend = sf;
    } else if (sf > 0 && (loop || (clip.flags & 0x01)) && clip.frameCount > 1) {
        // Looping: blend the last frame into the first.
        *b = &clip.frames[0];
        *blend = sf;
    }
    return true;
}

bool SkinMesh_CurrentFrames(const SkinAnimSet& set, const SkinAnimState& state,
                            const BakedBoneMatrix** a, const BakedBoneMatrix** b, uint16_t* blend) {
    return SkinPose_ClipFrames(set, state.currentClip, state.currentFrame, state.subFrame, state.loop,
                               a, b, blend);
}

bool SkinPose_Advance(const SkinAnimClip& clip, bool loop, uint16_t* frame, uint16_t* subFrame, int32_t dt12) {
    // advance = dt12 * fps / 30
    uint32_t advance = ((uint32_t)dt12 * (uint32_t)clip.fps) / 30u;

    uint32_t accum = (uint32_t)*subFrame + advance;

    uint16_t wholeFrames = (uint16_t)(accum >> 12);
    *subFrame = (uint16_t)(accum & 0xFFF);
    *frame += wholeFrames;

    if (*frame >= clip.frameCount) {
        if (loop || (clip.flags & 0x01)) {
            // Looping - wrap
            *frame = *frame % clip.frameCount;
        } else {
            // Stop at last frame
            *frame = clip.frameCount - 1;
            *subFrame = 0;
            return true;
        }
    }
    return false;
}

uint16_t SkinPose_FadeWeight(const SkinAnimState& state) {
    if (state.fadeLength == 0) return 4096;
    uint32_t w = state.fadePos / state.fadeLength;
    return w >= 4096 ? 4096 : (uint16_t)w;
}

// The renderer's frame lerp, kept to the exact expression and truncation:
// out = a + (((b - a) * w) >> 12), w in 4.12 (0..4096).
static inline void lerpBone(const BakedBoneMatrix& bA, const BakedBoneMatrix& bB, int32_t w,
                            BakedBoneMatrix& out) {
    for (int k = 0; k < 9; k++) {
        int32_t a = bA.r[k], b = bB.r[k];
        out.r[k] = (int16_t)(a + (((b - a) * w) >> 12));
    }
    for (int k = 0; k < 3; k++) {
        int32_t a = bA.t[k], b = bB.t[k];
        out.t[k] = (int16_t)(a + (((b - a) * w) >> 12));
    }
}

static bool computeRange(const SkinAnimSet& set, const SkinAnimState& state, int first, int end,
                         BakedBoneMatrix* out) {
    const BakedBoneMatrix* a;
    const BakedBoneMatrix* b;
    uint16_t sf;
    if (!SkinMesh_CurrentFrames(set, state, &a, &b, &sf)) return false;

    const BakedBoneMatrix* pa = nullptr;
    const BakedBoneMatrix* pb = nullptr;
    uint16_t psf = 0;
    int32_t w = 4096;
    bool fade = SkinPose_Fading(state) &&
                SkinPose_ClipFrames(set, state.prevClip, state.prevFrame, state.prevSubFrame,
                                    (state.prevFlags & SKINPOSE_PREV_LOOP) != 0, &pa, &pb, &psf);
    if (fade) w = SkinPose_FadeWeight(state);

    for (int bi = first; bi < end; bi++) {
        BakedBoneMatrix& o = out[bi - first];
        if (!fade) {
            // Exactly the renderer's old per-bone frame lerp.
            if (b) lerpBone(a[bi], b[bi], sf, o);
            else o = a[bi];
            continue;
        }
        BakedBoneMatrix cur, prev;
        if (b) lerpBone(a[bi], b[bi], sf, cur);
        else cur = a[bi];
        if (pb) lerpBone(pa[bi], pb[bi], psf, prev);
        else prev = pa[bi];
        lerpBone(prev, cur, w, o);
    }
    return true;
}

bool SkinPose_Compute(const SkinAnimSet& set, const SkinAnimState& state, BakedBoneMatrix* out) {
    int n = set.boneCount < SKINMESH_MAX_BONES ? set.boneCount : SKINMESH_MAX_BONES;
    return computeRange(set, state, 0, n, out);
}

bool SkinPose_ComputeBone(const SkinAnimSet& set, const SkinAnimState& state, int bone, BakedBoneMatrix* out) {
    if (bone < 0 || bone >= set.boneCount) return false;
    return computeRange(set, state, bone, bone + 1, out);
}

void SkinPose_BeginFade(const SkinAnimSet& set, SkinAnimState* state, uint16_t fadeFrames) {
    state->fadeLength = 0;
    state->fadePos = 0;
    if (fadeFrames == 0) return;

    // Only fade from something that was being drawn.
    const BakedBoneMatrix* a;
    const BakedBoneMatrix* b;
    uint16_t blend;
    if (!SkinMesh_CurrentFrames(set, *state, &a, &b, &blend)) return;

    // Two clips at most: a fade already in flight is dropped, and the old
    // current clip alone becomes the new source.
    state->prevClip = state->currentClip < set.clipCount ? state->currentClip : 0;
    state->prevFrame = state->currentFrame;
    state->prevSubFrame = state->subFrame;
    state->prevFlags = (uint8_t)((state->loop ? SKINPOSE_PREV_LOOP : 0) |
                                 (state->playing ? SKINPOSE_PREV_PLAYING : 0));
    state->fadeLength = fadeFrames;
}

void SkinPose_TickFade(SkinAnimState* state, int32_t dt12) {
    if (state->fadeLength == 0 || dt12 <= 0) return;

    const SkinAnimSet* set = state->animSet;
    if ((state->prevFlags & SKINPOSE_PREV_PLAYING) && set && state->prevClip < set->clipCount &&
        set->clips[state->prevClip].frameCount > 0) {
        // The previous clip never fires onComplete; a one-shot holds its last frame.
        if (SkinPose_Advance(set->clips[state->prevClip], (state->prevFlags & SKINPOSE_PREV_LOOP) != 0,
                             &state->prevFrame, &state->prevSubFrame, dt12)) {
            state->prevFlags &= (uint8_t)~SKINPOSE_PREV_PLAYING;
        }
    }

    state->fadePos += (uint32_t)dt12;
    if (state->fadePos >= ((uint32_t)state->fadeLength << 12)) {
        state->fadeLength = 0;
        state->fadePos = 0;
    }
}

}  // namespace psxsplash
