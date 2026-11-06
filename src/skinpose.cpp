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

// ---------------------------------------------------------------------------
// The crossfade blend. Poses up to ~180 degrees apart, so a plain matrix lerp
// would shrink the bone (a 135 degree fade collapses a forearm to a stub).
// ---------------------------------------------------------------------------

// x / 4096 rounded to nearest, halves away from zero.
static inline int32_t round12(int32_t x) { return x >= 0 ? (x + 2048) >> 12 : -((-x + 2048) >> 12); }

static inline int32_t fadeLerp(int32_t a, int32_t b, int32_t w) { return a + round12((b - a) * w); }

static inline int16_t clamp16(int32_t v) { return (int16_t)(v > 32767 ? 32767 : (v < -32768 ? -32768 : v)); }

// floor(sqrt(v)), shift-subtract, 16 iterations, no multiply or divide.
static uint32_t isqrt32(uint32_t v) {
    uint32_t r = 0;
    for (uint32_t bit = 1u << 30; bit; bit >>= 2) {
        if (v >= r + bit) {
            v -= r + bit;
            r = (r >> 1) + bit;
        } else {
            r >>= 1;
        }
    }
    return r;
}

// A blended axis shorter than 1/8 (squared, in 8.24) is too close to the
// poses being opposite to normalise: take the nearer pose's rotation.
static constexpr uint32_t kMinAxisLenSq = (4096u * 4096u) / 64u;

// Scale v (4.12) to unit length. One rsqrt: an integer sqrt and one divide.
// False when v is shorter than the guard (nothing written).
static bool normalise(int32_t v[3]) {
    uint32_t lenSq = 0;
    for (int i = 0; i < 3; i++) {
        uint32_t m = (uint32_t)(v[i] < 0 ? -v[i] : v[i]);  // < 2^16, so the sum < 2^32
        lenSq += m * m;
    }
    if (lenSq < kMinAxisLenSq) return false;
    // Scale lenSq up by 4^sh first so the integer sqrt keeps sh extra bits:
    // a short blended axis (down to 1/8) would otherwise lose up to 8 LSB.
    int sh = 0;
    while (sh < 6 && lenSq < (1u << 30)) {
        lenSq <<= 2;
        sh++;
    }
    uint32_t len = isqrt32(lenSq);       // 4.12 length * 2^sh, >= 2^15 when sh > 0
    uint32_t inv = (1u << 31) / len;
    int shift = 19 - sh;                 // v * inv >> shift = v * 4096 / (len / 2^sh)
    uint32_t half = 1u << (shift - 1);
    for (int i = 0; i < 3; i++) {
        // |v[i]| * 2^sh <= len, so the magnitude product is <= 2^31 and stays
        // in 32 bits (no 64-bit shifts: there is no libgcc for them here).
        uint32_t m = (uint32_t)(v[i] < 0 ? -v[i] : v[i]) * inv;
        int32_t r = (int32_t)((m + half) >> shift);
        v[i] = v[i] < 0 ? -r : r;
    }
    return true;
}

// Posed joint R * j + t, in the bone's model space. 64-bit sums: a corrupt
// (non-rotation) matrix must not overflow.
static inline int32_t round12_64(int64_t x) { return (int32_t)(x >= 0 ? (x + 2048) >> 12 : -((-x + 2048) >> 12)); }
static inline void posedJoint(const int16_t r[9], const int32_t t[3], const int16_t* j, int32_t out[3]) {
    for (int i = 0; i < 3; i++)
        out[i] = round12_64((int64_t)r[i * 3] * j[0] + (int64_t)r[i * 3 + 1] * j[1] +
                            (int64_t)r[i * 3 + 2] * j[2]) + t[i];
}

void SkinPose_FadeBone(const BakedBoneMatrix& prev, const BakedBoneMatrix& cur, int32_t w,
                       const int16_t* bind, BakedBoneMatrix* out) {
    // The ends are the poses themselves, untouched.
    if (w <= 0) { *out = prev; return; }
    if (w >= 4096) { *out = cur; return; }

    // Rotation: lerp, then rebuild an orthonormal basis. R maps a vertex as
    // R * v (renderer.cpp loads r row-major into Matrix33 for the GTE), so the
    // bone's axes are R's COLUMNS. Gram-Schmidt on columns 0 and 1, column 2
    // their cross product, signed to agree with the lerped column 2.
    int32_t c[3][3];  // c[col][row]
    for (int col = 0; col < 3; col++)
        for (int row = 0; row < 3; row++)
            c[col][row] = fadeLerp(prev.r[row * 3 + col], cur.r[row * 3 + col], w);

    bool ok = normalise(c[0]);
    if (ok) {
        int32_t d = round12(c[1][0] * c[0][0] + c[1][1] * c[0][1] + c[1][2] * c[0][2]);
        for (int i = 0; i < 3; i++) c[1][i] -= round12(d * c[0][i]);
        ok = normalise(c[1]);
    }
    if (ok) {
        int32_t x[3] = {
            round12(c[0][1] * c[1][2] - c[0][2] * c[1][1]),
            round12(c[0][2] * c[1][0] - c[0][0] * c[1][2]),
            round12(c[0][0] * c[1][1] - c[0][1] * c[1][0]),
        };
        int32_t s = x[0] * c[2][0] + x[1] * c[2][1] + x[2] * c[2][2];
        for (int i = 0; i < 3; i++) c[2][i] = s < 0 ? -x[i] : x[i];
        for (int col = 0; col < 3; col++)
            for (int row = 0; row < 3; row++) out->r[row * 3 + col] = clamp16(c[col][row]);
    } else {
        // Near-opposite poses: no meaningful blend axis. Hold the nearer pose's
        // rotation; the translation below still moves the joint smoothly.
        const BakedBoneMatrix& near = w < 2048 ? prev : cur;
        for (int k = 0; k < 9; k++) out->r[k] = near.r[k];
    }

    if (!bind) {
        for (int k = 0; k < 3; k++) out->t[k] = clamp16(fadeLerp(prev.t[k], cur.t[k], w));
        return;
    }
    // Translation: move the joint on the straight line between its two posed
    // positions and hang the rotated bone from it, so the limb stays rigid.
    int32_t tp[3] = {prev.t[0], prev.t[1], prev.t[2]};
    int32_t tc[3] = {cur.t[0], cur.t[1], cur.t[2]};
    int32_t zero[3] = {0, 0, 0};
    int32_t pp[3], pc[3], rj[3];
    posedJoint(prev.r, tp, bind, pp);
    posedJoint(cur.r, tc, bind, pc);
    posedJoint(out->r, zero, bind, rj);
    for (int k = 0; k < 3; k++)
        out->t[k] = clamp16(pp[k] + round12_64((int64_t)(pc[k] - pp[k]) * w) - rj[k]);
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
        SkinPose_FadeBone(prev, cur, w, set.bindPositions ? &set.bindPositions[bi * 3] : nullptr, &o);
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
