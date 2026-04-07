#include "interpolation.hh"

namespace psxsplash {

int32_t applyCurve(int32_t t, InterpMode mode) {
    switch (mode) {
        default:
        case InterpMode::Linear:
            return t;
        case InterpMode::Step:
            return 0;
        case InterpMode::EaseIn:
            return (int32_t)((int64_t)t * t >> 12);
        case InterpMode::EaseOut:
            return (int32_t)(((int64_t)t * (8192 - t)) >> 12);
        case InterpMode::EaseInOut: {
            int64_t t2 = (int64_t)t * t;
            int64_t t3 = t2 * t;
            return (int32_t)((3 * t2 - 2 * (t3 >> 12)) >> 12);
        }
    }
}

bool findKfPair(CutsceneKeyframe* kf, uint8_t count, uint16_t frame,
                uint8_t& a, uint8_t& b, int32_t& t, int16_t out[3]) {
    if (count == 0) {
        out[0] = out[1] = out[2] = 0;
        return false;
    }
    if (frame <= kf[0].getFrame() || count == 1) {
        out[0] = kf[0].values[0];
        out[1] = kf[0].values[1];
        out[2] = kf[0].values[2];
        return false;
    }
    if (frame >= kf[count - 1].getFrame()) {
        out[0] = kf[count - 1].values[0];
        out[1] = kf[count - 1].values[1];
        out[2] = kf[count - 1].values[2];
        return false;
    }
    b = 1;
    while (b < count && kf[b].getFrame() <= frame) b++;
    a = b - 1;
    uint16_t span = kf[b].getFrame() - kf[a].getFrame();
    if (span == 0) {
        out[0] = kf[a].values[0];
        out[1] = kf[a].values[1];
        out[2] = kf[a].values[2];
        return false;
    }
    uint32_t num = (uint32_t)(frame - kf[a].getFrame()) << 12;
    int32_t rawT = (int32_t)(num / span);
    t = applyCurve(rawT, kf[b].getInterp());
    return true;
}

void lerpKeyframes(CutsceneKeyframe* kf, uint8_t count, uint16_t frame,
                   const int16_t initial[3], int16_t out[3]) {
    uint8_t a, b;
    int32_t t;
    if (!findKfPair(kf, count, frame, a, b, t, out)) {
        if (count > 0 && kf[0].getFrame() > 0 && frame < kf[0].getFrame()) {
            uint16_t span = kf[0].getFrame();
            uint32_t num = (uint32_t)frame << 12;
            int32_t rawT = (int32_t)(num / span);
            int32_t ct = applyCurve(rawT, kf[0].getInterp());
            for (int i = 0; i < 3; i++) {
                int32_t delta = (int32_t)kf[0].values[i] - (int32_t)initial[i];
                out[i] = (int16_t)((int32_t)initial[i] + ((delta * ct) >> 12));
            }
        }
        return;
    }

    for (int i = 0; i < 3; i++) {
        int32_t delta = (int32_t)kf[b].values[i] - (int32_t)kf[a].values[i];
        out[i] = (int16_t)((int32_t)kf[a].values[i] + ((delta * t) >> 12));
    }
}

static constexpr int32_t ANGLE_FULL_CIRCLE = 2048;
static constexpr int32_t ANGLE_HALF_CIRCLE = 1024;

void lerpAngles(CutsceneKeyframe* kf, uint8_t count, uint16_t frame,
                const int16_t initial[3], int16_t out[3]) {
    uint8_t a, b;
    int32_t t;
    if (!findKfPair(kf, count, frame, a, b, t, out)) {
        if (count > 0 && kf[0].getFrame() > 0 && frame < kf[0].getFrame()) {
            uint16_t span = kf[0].getFrame();
            uint32_t num = (uint32_t)frame << 12;
            int32_t rawT = (int32_t)(num / span);
            int32_t ct = applyCurve(rawT, kf[0].getInterp());
            for (int i = 0; i < 3; i++) {
                int32_t from = (int32_t)initial[i];
                int32_t to   = (int32_t)kf[0].values[i];
                int32_t delta = to - from;
                delta = ((delta + ANGLE_HALF_CIRCLE) % ANGLE_FULL_CIRCLE + ANGLE_FULL_CIRCLE) % ANGLE_FULL_CIRCLE - ANGLE_HALF_CIRCLE;
                out[i] = (int16_t)(from + ((delta * ct) >> 12));
            }
        }
        return;
    }

    for (int i = 0; i < 3; i++) {
        int32_t from = (int32_t)kf[a].values[i];
        int32_t to   = (int32_t)kf[b].values[i];
        int32_t delta = to - from;
        delta = ((delta + ANGLE_HALF_CIRCLE) % ANGLE_FULL_CIRCLE + ANGLE_FULL_CIRCLE) % ANGLE_FULL_CIRCLE - ANGLE_HALF_CIRCLE;
        out[i] = (int16_t)(from + ((delta * t) >> 12));
    }
}

// ============================================================================
// Sub-frame precision variants (frame + 0.12 fp subFrame)
// ============================================================================

bool findKfPairSub(CutsceneKeyframe* kf, uint8_t count, uint16_t frame, uint16_t subFrame,
                   uint8_t& a, uint8_t& b, int32_t& t, int16_t out[3]) {
    if (count == 0) {
        out[0] = out[1] = out[2] = 0;
        return false;
    }
    if ((frame < kf[0].getFrame() || (frame == kf[0].getFrame() && subFrame == 0)) || count == 1) {
        if (frame < kf[0].getFrame() || (frame == kf[0].getFrame() && subFrame == 0)) {
            out[0] = kf[0].values[0];
            out[1] = kf[0].values[1];
            out[2] = kf[0].values[2];
            return false;
        }
    }
    if (frame > kf[count - 1].getFrame() ||
        (frame == kf[count - 1].getFrame() && subFrame > 0)) {
        out[0] = kf[count - 1].values[0];
        out[1] = kf[count - 1].values[1];
        out[2] = kf[count - 1].values[2];
        return false;
    }
    b = 1;
    while (b < count && kf[b].getFrame() <= frame) b++;
    a = b - 1;
    uint16_t spanFrames = kf[b].getFrame() - kf[a].getFrame();
    if (spanFrames == 0) {
        out[0] = kf[a].values[0];
        out[1] = kf[a].values[1];
        out[2] = kf[a].values[2];
        return false;
    }
    // Compute t with sub-frame precision: ((dist * 4096 + subFrame) / span)
    uint32_t dist = (uint32_t)(frame - kf[a].getFrame());
    uint32_t num = dist * 4096u + (uint32_t)subFrame;
    int32_t rawT = (int32_t)(num / (uint32_t)spanFrames);
    if (rawT > 4096) rawT = 4096;
    t = applyCurve(rawT, kf[b].getInterp());
    return true;
}

void lerpKeyframesSub(CutsceneKeyframe* kf, uint8_t count, uint16_t frame, uint16_t subFrame,
                      const int16_t initial[3], int16_t out[3]) {
    uint8_t a, b;
    int32_t t;
    if (!findKfPairSub(kf, count, frame, subFrame, a, b, t, out)) {
        if (count > 0 && kf[0].getFrame() > 0 && frame < kf[0].getFrame()) {
            uint16_t span = kf[0].getFrame();
            uint32_t num = (uint32_t)frame * 4096u + (uint32_t)subFrame;
            int32_t rawT = (int32_t)(num / (uint32_t)span);
            if (rawT > 4096) rawT = 4096;
            int32_t ct = applyCurve(rawT, kf[0].getInterp());
            for (int i = 0; i < 3; i++) {
                int32_t delta = (int32_t)kf[0].values[i] - (int32_t)initial[i];
                out[i] = (int16_t)((int32_t)initial[i] + ((delta * ct) >> 12));
            }
        }
        return;
    }

    for (int i = 0; i < 3; i++) {
        int32_t delta = (int32_t)kf[b].values[i] - (int32_t)kf[a].values[i];
        out[i] = (int16_t)((int32_t)kf[a].values[i] + ((delta * t) >> 12));
    }
}

void lerpAnglesSub(CutsceneKeyframe* kf, uint8_t count, uint16_t frame, uint16_t subFrame,
                   const int16_t initial[3], int16_t out[3]) {
    uint8_t a, b;
    int32_t t;
    if (!findKfPairSub(kf, count, frame, subFrame, a, b, t, out)) {
        if (count > 0 && kf[0].getFrame() > 0 && frame < kf[0].getFrame()) {
            uint16_t span = kf[0].getFrame();
            uint32_t num = (uint32_t)frame * 4096u + (uint32_t)subFrame;
            int32_t rawT = (int32_t)(num / (uint32_t)span);
            if (rawT > 4096) rawT = 4096;
            int32_t ct = applyCurve(rawT, kf[0].getInterp());
            for (int i = 0; i < 3; i++) {
                int32_t from = (int32_t)initial[i];
                int32_t to   = (int32_t)kf[0].values[i];
                int32_t delta = to - from;
                delta = ((delta + ANGLE_HALF_CIRCLE) % ANGLE_FULL_CIRCLE + ANGLE_FULL_CIRCLE) % ANGLE_FULL_CIRCLE - ANGLE_HALF_CIRCLE;
                out[i] = (int16_t)(from + ((delta * ct) >> 12));
            }
        }
        return;
    }

    for (int i = 0; i < 3; i++) {
        int32_t from = (int32_t)kf[a].values[i];
        int32_t to   = (int32_t)kf[b].values[i];
        int32_t delta = to - from;
        delta = ((delta + ANGLE_HALF_CIRCLE) % ANGLE_FULL_CIRCLE + ANGLE_FULL_CIRCLE) % ANGLE_FULL_CIRCLE - ANGLE_HALF_CIRCLE;
        out[i] = (int16_t)(from + ((delta * t) >> 12));
    }
}

}  // namespace psxsplash
