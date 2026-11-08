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

// Quaternions in 2.14 (16384 = 1.0), order w, x, y, z.
static constexpr int32_t kQ1 = 16384;

static inline int32_t clampQ(int32_t v) { return v > 2 * kQ1 ? 2 * kQ1 : (v < -2 * kQ1 ? -2 * kQ1 : v); }

// round(a * b / 2^sh) for the constant shifts used here, halves away from
// zero. Constant 64-bit shifts inline; variable ones would need libgcc.
static inline int32_t mulShift16(int32_t a, int32_t b) {
    int64_t p = (int64_t)a * b;
    return (int32_t)(p >= 0 ? (p + 32768) >> 16 : -((-p + 32768) >> 16));
}
static inline int32_t mulShift31(int64_t a, int32_t b) {
    int64_t p = a * b;
    return (int32_t)(p >= 0 ? (p + (1ll << 30)) >> 31 : -((-p + (1ll << 30)) >> 31));
}

// Rotation matrix (4.12, row-major, applied as R * v) to a unit quaternion.
// The largest of w, x, y, z comes from the trace or the largest diagonal
// entry (one integer sqrt), the other three from off-diagonal sums divided
// by four times it (one divide, as a reciprocal). False when no component
// reaches 1/4, which a rotation always has: the matrix is not one.
static bool matToQuat(const int16_t* m, int32_t q[4]) {
    int32_t m00 = m[0], m01 = m[1], m02 = m[2];
    int32_t m10 = m[3], m11 = m[4], m12 = m[5];
    int32_t m20 = m[6], m21 = m[7], m22 = m[8];
    int32_t t = m00 + m11 + m22;
    int big;
    int32_t r;  // 4.12: 1 + trace, or 1 + one diagonal entry - the other two
    if (t > 0) {
        big = 0; r = 4096 + t;
    } else if (m00 >= m11 && m00 >= m22) {
        big = 1; r = 4096 + m00 - m11 - m22;
    } else if (m11 >= m22) {
        big = 2; r = 4096 + m11 - m00 - m22;
    } else {
        big = 3; r = 4096 + m22 - m00 - m11;
    }
    if (r <= 0) return false;
    // The big component sqrt(r) / 2 in 2.14 is sqrt(r_4.12 * 2^14); r < 2^17.
    int32_t c = (int32_t)isqrt32((uint32_t)r << 14);
    if (c < kQ1 / 4) return false;
    // The others are d / (4 c): in 2.14 that is d_4.12 * 2^14 / c_2.14.
    int32_t inv = (1 << 30) / c;
    switch (big) {
        case 0:
            q[0] = c;
            q[1] = mulShift16(m21 - m12, inv);
            q[2] = mulShift16(m02 - m20, inv);
            q[3] = mulShift16(m10 - m01, inv);
            break;
        case 1:
            q[1] = c;
            q[0] = mulShift16(m21 - m12, inv);
            q[2] = mulShift16(m01 + m10, inv);
            q[3] = mulShift16(m02 + m20, inv);
            break;
        case 2:
            q[2] = c;
            q[0] = mulShift16(m02 - m20, inv);
            q[1] = mulShift16(m01 + m10, inv);
            q[3] = mulShift16(m12 + m21, inv);
            break;
        default:
            q[3] = c;
            q[0] = mulShift16(m10 - m01, inv);
            q[1] = mulShift16(m02 + m20, inv);
            q[2] = mulShift16(m12 + m21, inv);
            break;
    }
    for (int i = 0; i < 4; i++) q[i] = clampQ(q[i]);
    return true;
}

// Quaternion (2.14, any non-zero length n) to a 4.12 rotation matrix, same
// convention. Dividing the products by n folds the normalisation in: no sqrt,
// one divide. False when |q|^2 < 1/8 (degenerate).
static bool quatToMat(const int32_t q[4], int16_t* m) {
    int64_t w = q[0], x = q[1], y = q[2], z = q[3];
    int64_t n = w * w + x * x + y * y + z * z;  // 4.28
    int32_t nq = (int32_t)(n >> 14);            // 2.14
    if (nq < kQ1 / 8) return false;
    int32_t rcp = (1 << 30) / nq;
    // entry_4.12 = 4096 * 2P/n = P_4.28 * rcp >> 31.
    m[0] = clamp16(4096 - mulShift31(y * y + z * z, rcp));
    m[1] = clamp16(mulShift31(x * y - w * z, rcp));
    m[2] = clamp16(mulShift31(x * z + w * y, rcp));
    m[3] = clamp16(mulShift31(x * y + w * z, rcp));
    m[4] = clamp16(4096 - mulShift31(x * x + z * z, rcp));
    m[5] = clamp16(mulShift31(y * z - w * x, rcp));
    m[6] = clamp16(mulShift31(x * z - w * y, rcp));
    m[7] = clamp16(mulShift31(y * z + w * x, rcp));
    m[8] = clamp16(4096 - mulShift31(x * x + y * y, rcp));
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

    // Rotation: quaternion nlerp. Both poses to unit quaternions, the second
    // negated if needed so the blend takes the shortest path (this is also
    // what keeps poses near 180 degrees apart well defined), lerp, and back to
    // a matrix with the normalisation folded in. Stays on the great arc for
    // any rotation axis.
    int32_t qa[4], qb[4], q[4];
    bool ok = matToQuat(prev.r, qa) && matToQuat(cur.r, qb);
    if (ok) {
        int64_t dot = 0;
        for (int i = 0; i < 4; i++) dot += (int64_t)qa[i] * qb[i];
        if (dot < 0)
            for (int i = 0; i < 4; i++) qb[i] = -qb[i];
        for (int i = 0; i < 4; i++) q[i] = fadeLerp(qa[i], qb[i], w);
        ok = quatToMat(q, out->r);
    }
    if (!ok) {
        // Not rotations (or a degenerate blend): hold the nearer pose's
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
