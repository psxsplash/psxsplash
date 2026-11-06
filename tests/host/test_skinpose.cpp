// Host tests for the skinned-animation pose math (src/skinpose.cpp), compiled
// natively: the frame lerp, the crossfade from a previous clip, and the clip
// and fade advance that SkinMesh_Tick and SkinnedAnim.Play drive.

#include "skinpose.hh"

#include <stdlib.h>

#include "testing.hh"

using namespace psxsplash;

namespace {

constexpr int kBones = 3;

// clip 0 "idle": 3 frames, 30 fps, no loop flag.
// clip 1 "run":  4 frames, 15 fps, no loop flag.
// clip 2 "wave": 2 frames, 30 fps, loop flag set in the clip.
BakedBoneMatrix g_idle[3 * kBones];
BakedBoneMatrix g_run[4 * kBones];
BakedBoneMatrix g_wave[2 * kBones];

void fillFrames(BakedBoneMatrix* f, int frames, int seed) {
    for (int i = 0; i < frames * kBones; i++) {
        for (int k = 0; k < 9; k++) f[i].r[k] = (int16_t)(((i + 1) * 977 + k * 331 + seed * 1543) % 8192 - 4096);
        for (int k = 0; k < 3; k++) f[i].t[k] = (int16_t)(((i + 3) * 2711 + k * 619 + seed * 4099) % 60000 - 30000);
    }
}

SkinAnimSet makeSet() {
    fillFrames(g_idle, 3, 1);
    fillFrames(g_run, 4, 2);
    fillFrames(g_wave, 2, 3);
    SkinAnimSet s{};
    s.clipCount = 3;
    s.boneCount = kBones;
    s.clips[0] = {"idle", g_idle, 3, 0x00, 30, kBones, {}};
    s.clips[1] = {"run", g_run, 4, 0x00, 15, kBones, {}};
    s.clips[2] = {"wave", g_wave, 2, 0x01, 30, kBones, {}};
    return s;
}

SkinAnimState makeState(SkinAnimSet* set, uint8_t clip, uint16_t frame, uint16_t sub, bool loop = false) {
    SkinAnimState st{};
    st.animSet = set;
    st.currentClip = clip;
    st.currentFrame = frame;
    st.subFrame = sub;
    st.playing = true;
    st.loop = loop;
    return st;
}

// What SkinnedAnim_Play does to the state, in the same order.
void play(SkinAnimSet& set, SkinAnimState& st, uint8_t clip, bool loop, uint16_t fade) {
    SkinPose_BeginFade(set, &st, fade);
    st.currentClip = clip;
    st.currentFrame = 0;
    st.subFrame = 0;
    st.playing = true;
    st.loop = loop;
}

// ---- Oracle: the pre-crossfade code, copied verbatim (frame selection from
// SkinMesh_CurrentFrames, the lerp from renderer.cpp). Do not "fix" it.
bool oldCurrentFrames(const SkinAnimSet& set, const SkinAnimState& state,
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
        *b = &clip.frames[0];
        *blend = sf;
    }
    return true;
}

// Renderer's drawn pose before this change (zero-copy path copied into out).
void oldPose(const SkinAnimSet& set, const SkinAnimState& state, BakedBoneMatrix* lerpedBones) {
    const BakedBoneMatrix* boneMatricesA;
    const BakedBoneMatrix* boneMatricesB;
    uint16_t sf;
    if (!oldCurrentFrames(set, state, &boneMatricesA, &boneMatricesB, &sf)) abort();
    if (boneMatricesB) {
        for (int bi = 0; bi < set.boneCount && bi < SKINMESH_MAX_BONES; bi++) {
            const BakedBoneMatrix& bA = boneMatricesA[bi];
            const BakedBoneMatrix& bB = boneMatricesB[bi];
            BakedBoneMatrix& out = lerpedBones[bi];
            for (int k = 0; k < 9; k++) {
                int32_t a = bA.r[k], b = bB.r[k];
                out.r[k] = (int16_t)(a + (((b - a) * sf) >> 12));
            }
            for (int k = 0; k < 3; k++) {
                int32_t a = bA.t[k], b = bB.t[k];
                out.t[k] = (int16_t)(a + (((b - a) * sf) >> 12));
            }
        }
    } else {
        for (int bi = 0; bi < set.boneCount; bi++) lerpedBones[bi] = boneMatricesA[bi];
    }
}

// The pose a single clip draws at (frame, sub), via the oracle.
void clipPose(const SkinAnimSet& set, uint8_t clip, uint16_t frame, uint16_t sub, bool loop,
              BakedBoneMatrix* out) {
    SkinAnimSet copy = set;
    SkinAnimState st = makeState(&copy, clip, frame, sub, loop);
    oldPose(set, st, out);
}

// The pre-crossfade SkinMesh_Tick advance, copied verbatim minus the callback.
// Returns true where the old code stopped the clip.
bool oldTick(SkinAnimState* state, int32_t dt12) {
    const SkinAnimClip& clip = state->animSet->clips[state->currentClip];
    uint32_t advance = ((uint32_t)dt12 * (uint32_t)clip.fps) / 30u;
    uint32_t accum = (uint32_t)state->subFrame + advance;
    uint16_t wholeFrames = (uint16_t)(accum >> 12);
    state->subFrame = (uint16_t)(accum & 0xFFF);
    state->currentFrame += wholeFrames;
    if (state->currentFrame >= clip.frameCount) {
        if (state->loop || (clip.flags & 0x01)) {
            state->currentFrame = state->currentFrame % clip.frameCount;
        } else {
            state->currentFrame = clip.frameCount - 1;
            state->subFrame = 0;
            state->playing = false;
            return true;
        }
    }
    return false;
}

int maxDiff(const BakedBoneMatrix* a, const BakedBoneMatrix* b, int n) {
    int m = 0;
    for (int i = 0; i < n; i++) {
        for (int k = 0; k < 9; k++) m = abs(a[i].r[k] - b[i].r[k]) > m ? abs(a[i].r[k] - b[i].r[k]) : m;
        for (int k = 0; k < 3; k++) m = abs(a[i].t[k] - b[i].t[k]) > m ? abs(a[i].t[k] - b[i].t[k]) : m;
    }
    return m;
}

// A state mid-fade: current = run @ (1, 1000), previous = idle @ (0, 3000).
SkinAnimState fadingState(SkinAnimSet* set, uint16_t fadeLength, uint32_t fadePos) {
    SkinAnimState st = makeState(set, 1, 1, 1000);
    st.prevClip = 0;
    st.prevFrame = 0;
    st.prevSubFrame = 3000;
    st.prevFlags = SKINPOSE_PREV_PLAYING;
    st.fadeLength = fadeLength;
    st.fadePos = fadePos;
    return st;
}

}  // namespace

// ---------------------------------------------------------------------------
// fade 0: byte-for-byte the old pose
// ---------------------------------------------------------------------------

TEST(fade0_equals_old_two_frame_lerp) {
    SkinAnimSet set = makeSet();
    const uint16_t subs[] = {0, 1, 1000, 2048, 4095};
    int cases = 0;
    for (uint8_t clip = 0; clip < 3; clip++) {
        for (uint16_t frame = 0; frame < 5; frame++) {  // 4 is past the end of every clip
            for (uint16_t sub : subs) {
                for (int loop = 0; loop < 2; loop++) {
                    SkinAnimState st = makeState(&set, clip, frame, sub, loop != 0);
                    BakedBoneMatrix got[kBones], want[kBones];
                    CHECK(SkinPose_Compute(set, st, got));
                    oldPose(set, st, want);
                    if (!CHECK_MEM(got, want, sizeof(got))) return;
                    for (int bi = 0; bi < kBones; bi++) {
                        BakedBoneMatrix one;
                        CHECK(SkinPose_ComputeBone(set, st, bi, &one));
                        CHECK_MEM(&one, &want[bi], sizeof(one));
                    }
                    cases++;
                }
            }
        }
    }
    CHECK_EQ(cases, 3 * 5 * 5 * 2);
}

TEST(fade0_current_frames_match_old_selection) {
    SkinAnimSet set = makeSet();
    for (uint8_t clip = 0; clip < 4; clip++) {  // 3 is out of range -> clip 0
        for (uint16_t frame = 0; frame < 5; frame++) {
            const uint16_t subs[] = {0, 7, 4095};
            for (uint16_t sub : subs) {
                SkinAnimState st = makeState(&set, clip, frame, sub, frame & 1);
                const BakedBoneMatrix *a1, *b1, *a2, *b2;
                uint16_t w1, w2;
                CHECK_EQ(SkinMesh_CurrentFrames(set, st, &a1, &b1, &w1), oldCurrentFrames(set, st, &a2, &b2, &w2));
                CHECK(a1 == a2);
                CHECK(b1 == b2);
                CHECK_EQ(w1, w2);
            }
        }
    }
}

TEST(advance_matches_old_tick) {
    SkinAnimSet set = makeSet();
    const int32_t dts[] = {0, 1, 1365, 4096, 4097, 8192, 20000};
    for (uint8_t clip = 0; clip < 3; clip++) {
        for (int loop = 0; loop < 2; loop++) {
            for (int32_t dt : dts) {
                SkinAnimState o = makeState(&set, clip, 0, 0, loop != 0);
                uint16_t frame = 0, sub = 0;
                for (int t = 0; t < 12 && o.playing; t++) {
                    bool endedOld = oldTick(&o, dt);
                    bool ended = SkinPose_Advance(set.clips[clip], loop != 0, &frame, &sub, dt);
                    CHECK_EQ(ended, endedOld);
                    CHECK_EQ(frame, o.currentFrame);
                    if (!CHECK_EQ(sub, o.subFrame)) return;
                }
            }
        }
    }
}

TEST(no_fade_without_play_fade) {
    SkinAnimSet set = makeSet();
    SkinAnimState st = makeState(&set, 0, 1, 0);
    play(set, st, 1, false, 0);
    CHECK(!SkinPose_Fading(st));
    CHECK_EQ(st.fadePos, 0);
}

// ---------------------------------------------------------------------------
// The crossfade weight
// ---------------------------------------------------------------------------

TEST(weight0_equals_previous_pose) {
    SkinAnimSet set = makeSet();
    SkinAnimState st = fadingState(&set, 8, 0);
    CHECK_EQ(SkinPose_FadeWeight(st), 0);
    BakedBoneMatrix got[kBones], want[kBones];
    CHECK(SkinPose_Compute(set, st, got));
    clipPose(set, 0, 0, 3000, false, want);
    CHECK_MEM(got, want, sizeof(got));
}

TEST(weight_at_fade_length_equals_current_pose) {
    SkinAnimSet set = makeSet();
    SkinAnimState st = fadingState(&set, 8, 8u << 12);
    CHECK_EQ(SkinPose_FadeWeight(st), 4096);
    BakedBoneMatrix got[kBones], want[kBones];
    CHECK(SkinPose_Compute(set, st, got));
    clipPose(set, 1, 1, 1000, false, want);
    CHECK_MEM(got, want, sizeof(got));
}

TEST(halfway_is_midpoint_within_one_lsb) {
    SkinAnimSet set = makeSet();
    SkinAnimState st = fadingState(&set, 8, 4u << 12);
    CHECK_EQ(SkinPose_FadeWeight(st), 2048);
    BakedBoneMatrix got[kBones], prev[kBones], cur[kBones], mid[kBones];
    CHECK(SkinPose_Compute(set, st, got));
    clipPose(set, 0, 0, 3000, false, prev);
    clipPose(set, 1, 1, 1000, false, cur);
    for (int bi = 0; bi < kBones; bi++) {
        for (int k = 0; k < 9; k++) mid[bi].r[k] = (int16_t)((prev[bi].r[k] + cur[bi].r[k]) / 2);
        for (int k = 0; k < 3; k++) mid[bi].t[k] = (int16_t)((prev[bi].t[k] + cur[bi].t[k]) / 2);
    }
    CHECK(maxDiff(got, mid, kBones) <= 1);
    // And it really is between the two, not equal to either.
    CHECK(maxDiff(got, prev, kBones) > 1);
    CHECK(maxDiff(got, cur, kBones) > 1);
}

TEST(compute_bone_matches_compute_mid_fade) {
    SkinAnimSet set = makeSet();
    SkinAnimState st = fadingState(&set, 8, 3u << 12);
    BakedBoneMatrix all[kBones];
    CHECK(SkinPose_Compute(set, st, all));
    for (int bi = 0; bi < kBones; bi++) {
        BakedBoneMatrix one;
        CHECK(SkinPose_ComputeBone(set, st, bi, &one));
        CHECK_MEM(&one, &all[bi], sizeof(one));
    }
    BakedBoneMatrix dummy;
    CHECK(!SkinPose_ComputeBone(set, st, kBones, &dummy));
    CHECK(!SkinPose_ComputeBone(set, st, -1, &dummy));
}

// ---------------------------------------------------------------------------
// The previous clip's playback
// ---------------------------------------------------------------------------

TEST(looping_prev_clip_wraps_last_to_first) {
    SkinAnimSet set = makeSet();
    // idle has no loop flag; the runtime loop override on the old clip carries over.
    SkinAnimState st = makeState(&set, 0, 2, 0, true);
    play(set, st, 1, false, 30);
    CHECK_EQ(st.prevClip, 0);
    CHECK(st.prevFlags & SKINPOSE_PREV_LOOP);
    // Half a frame on: the previous pose blends idle's last frame into its first.
    SkinPose_TickFade(&st, 2048);
    CHECK_EQ(st.prevFrame, 2);
    CHECK_EQ(st.prevSubFrame, 2048);
    st.fadePos = 0;  // look at the previous pose alone
    BakedBoneMatrix got[kBones], want[kBones];
    CHECK(SkinPose_Compute(set, st, got));
    for (int bi = 0; bi < kBones; bi++) {
        const BakedBoneMatrix& a = g_idle[2 * kBones + bi];
        const BakedBoneMatrix& b = g_idle[0 * kBones + bi];
        for (int k = 0; k < 9; k++) want[bi].r[k] = (int16_t)(a.r[k] + (((b.r[k] - a.r[k]) * 2048) >> 12));
        for (int k = 0; k < 3; k++) want[bi].t[k] = (int16_t)(a.t[k] + (((b.t[k] - a.t[k]) * 2048) >> 12));
    }
    CHECK_MEM(got, want, sizeof(got));
    // Another half frame wraps the frame index itself.
    SkinPose_TickFade(&st, 2048);
    CHECK_EQ(st.prevFrame, 0);
    CHECK_EQ(st.prevSubFrame, 0);
    CHECK(st.prevFlags & SKINPOSE_PREV_PLAYING);
}

TEST(non_looping_prev_clip_holds_last_frame) {
    SkinAnimSet set = makeSet();
    SkinAnimState st = makeState(&set, 0, 1, 0, false);
    play(set, st, 1, false, 20);
    CHECK(!(st.prevFlags & SKINPOSE_PREV_LOOP));
    for (int t = 0; t < 5; t++) SkinPose_TickFade(&st, 4096);  // idle is 3 frames
    CHECK(SkinPose_Fading(st));
    CHECK_EQ(st.prevFrame, 2);
    CHECK_EQ(st.prevSubFrame, 0);
    CHECK(!(st.prevFlags & SKINPOSE_PREV_PLAYING));
    st.fadePos = 0;
    BakedBoneMatrix got[kBones];
    CHECK(SkinPose_Compute(set, st, got));
    CHECK_MEM(got, &g_idle[2 * kBones], sizeof(got));
}

TEST(stopped_prev_clip_does_not_advance) {
    SkinAnimSet set = makeSet();
    SkinAnimState st = makeState(&set, 1, 2, 500, false);
    st.playing = false;  // frozen by Stop or a finished one-shot
    play(set, st, 0, false, 10);
    CHECK(SkinPose_Fading(st));
    for (int t = 0; t < 4; t++) SkinPose_TickFade(&st, 4096);
    CHECK_EQ(st.prevFrame, 2);
    CHECK_EQ(st.prevSubFrame, 500);
}

// ---------------------------------------------------------------------------
// Fade lifetime
// ---------------------------------------------------------------------------

TEST(tick_ends_fade_at_fade_length) {
    SkinAnimSet set = makeSet();
    SkinAnimState st = makeState(&set, 0, 0, 0, true);
    play(set, st, 1, true, 8);
    CHECK(SkinPose_Fading(st));
    CHECK_EQ(SkinPose_FadeWeight(st), 0);
    for (int t = 1; t < 8; t++) {
        SkinPose_TickFade(&st, 4096);
        if (!CHECK(SkinPose_Fading(st))) return;
        CHECK_EQ(SkinPose_FadeWeight(st), t * 512);
    }
    SkinPose_TickFade(&st, 4096);
    CHECK(!SkinPose_Fading(st));
    CHECK_EQ(st.fadePos, 0);
    // Once over, the pose is the current clip alone, byte for byte.
    BakedBoneMatrix got[kBones], want[kBones];
    CHECK(SkinPose_Compute(set, st, got));
    oldPose(set, st, want);
    CHECK_MEM(got, want, sizeof(got));
}

TEST(tick_ends_fade_framerate_independent) {
    SkinAnimSet set = makeSet();
    SkinAnimState st = makeState(&set, 0, 0, 0);
    play(set, st, 1, false, 3);
    int ticks = 0;
    while (SkinPose_Fading(st) && ticks < 100) {
        SkinPose_TickFade(&st, 2048);  // 60 fps
        ticks++;
    }
    CHECK_EQ(ticks, 6);
}

TEST(second_play_mid_fade_fades_from_old_current) {
    SkinAnimSet set = makeSet();
    SkinAnimState st = makeState(&set, 0, 0, 0, true);
    play(set, st, 1, true, 8);  // idle -> run
    for (int t = 0; t < 3; t++) {
        SkinPose_TickFade(&st, 4096);
        // What SkinMesh_Tick does to the current clip.
        SkinPose_Advance(set.clips[st.currentClip], st.loop, &st.currentFrame, &st.subFrame, 4096);
    }
    uint16_t runFrame = st.currentFrame, runSub = st.subFrame;
    CHECK(runFrame == 1 && runSub == 2048);  // run is 15 fps: 1.5 frames after 3 ticks
    play(set, st, 2, false, 4);  // run -> wave, mid-fade
    CHECK(SkinPose_Fading(st));
    CHECK_EQ(st.fadeLength, 4);
    CHECK_EQ(st.fadePos, 0);
    CHECK_EQ(st.prevClip, 1);
    CHECK_EQ(st.prevFrame, runFrame);
    CHECK_EQ(st.prevSubFrame, runSub);
    // The in-flight idle blend is dropped: weight 0 is run's pose alone.
    BakedBoneMatrix got[kBones], want[kBones];
    CHECK(SkinPose_Compute(set, st, got));
    clipPose(set, 1, runFrame, runSub, true, want);
    CHECK_MEM(got, want, sizeof(got));
}

TEST(play_fade0_cancels_running_fade) {
    SkinAnimSet set = makeSet();
    SkinAnimState st = makeState(&set, 0, 0, 0);
    play(set, st, 1, false, 8);
    SkinPose_TickFade(&st, 4096);
    play(set, st, 2, false, 0);
    CHECK(!SkinPose_Fading(st));
    BakedBoneMatrix got[kBones], want[kBones];
    CHECK(SkinPose_Compute(set, st, got));
    oldPose(set, st, want);
    CHECK_MEM(got, want, sizeof(got));
}

TEST(no_fade_from_nothing_drawn) {
    SkinAnimSet set = makeSet();
    set.clips[0].frames = nullptr;  // clip 0 draws nothing
    SkinAnimState st = makeState(&set, 0, 0, 0);
    play(set, st, 1, false, 8);
    CHECK(!SkinPose_Fading(st));
}

int main() { return psxsplash::test::runAll(); }
