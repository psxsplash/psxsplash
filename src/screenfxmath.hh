#pragma once

// Screen effects state: fades, flash, shake, letterbox and trail, advanced by
// the frame time and read by the renderer (screenfx.cpp). Pure integer code
// with no psyqo dependency, so the host tests compile it natively.
//
// Durations are in 30 fps frames, the unit cutscene keyframes use; tick() takes
// dt12, 4096 per 30 fps frame. Nothing here needs a 64-bit multiply or divide.

#include <stdint.h>

namespace psxsplash::screenfx {

static constexpr int32_t kMaxFrames = 8191;  // a cutscene's longest timeline

// A value moving from `from` to `to` over `frames` frames, linearly.
struct Ramp {
    int16_t from = 0, to = 0, value = 0;
    int32_t frames = 0;  // 0: settled at value
    int32_t elapsed12 = 0;

    void set(int32_t v) {
        from = to = value = (int16_t)v;
        frames = 0;
        elapsed12 = 0;
    }
    void start(int32_t target, int32_t f) {
        if (f > kMaxFrames) f = kMaxFrames;
        if (f <= 0 || target == value) {
            set(target);
            return;
        }
        from = value;
        to = (int16_t)target;
        frames = f;
        elapsed12 = 0;
    }
    bool running() const { return frames > 0; }
    void tick(int32_t dt12) {
        if (frames <= 0 || dt12 <= 0) return;
        elapsed12 += dt12;
        if (elapsed12 >= frames * 4096) {
            set(to);
            return;
        }
        // Progress in 1/256: elapsed12 / (frames * 4096) * 256.
        const int32_t p = elapsed12 / (frames * 16);
        value = (int16_t)(from + (((int32_t)to - from) * p >> 8));
    }
};

// Which effects a cutscene wrote, so stopping it can put them back.
enum Driven : uint8_t {
    kDrivenFade = 1 << 0,
    kDrivenFadeColor = 1 << 1,
    kDrivenLetterbox = 1 << 2,
    kDrivenShake = 1 << 3,
    kDrivenTrail = 1 << 4,
};

inline uint32_t hash32(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

// The shake offset for one step: x alternates sides every step, y picks a side
// at random, and both lie in (amp / 2, amp] so consecutive frames always move.
inline void shakeOffset(uint32_t step, int32_t amp, int32_t& dx, int32_t& dy) {
    if (amp <= 0) {
        dx = dy = 0;
        return;
    }
    const uint32_t h = hash32(step);
    const int32_t mx = amp - (int32_t)(((h & 0xff) * (uint32_t)amp) >> 9);
    const int32_t my = amp - (int32_t)((((h >> 16) & 0xff) * (uint32_t)amp) >> 9);
    dx = (step & 1) ? mx : -mx;
    dy = (h & 0x100) ? my : -my;
}

// c * a / 255, exact at a = 0 and a = 255.
inline uint8_t scale255(uint8_t c, int32_t a) {
    if (a <= 0) return 0;
    if (a >= 255) return c;
    return (uint8_t)(((uint32_t)c * (uint32_t)a + 127) / 255);
}

struct State {
    // Fade: covers everything, UI and text included. amount 0..255 (255 = the
    // screen is exactly the fade colour).
    Ramp fade;
    uint8_t fadeR = 0, fadeG = 0, fadeB = 0;
    // Flash: additive colour, fading back to nothing. 255 at its start.
    Ramp flash;
    uint8_t flashR = 0, flashG = 0, flashB = 0;
    // Shake: amplitude in pixels, decaying to 0.
    Ramp shake;
    uint32_t shakeStep = 0;
    // Letterbox: height of each bar in pixels.
    Ramp letterbox;
    // Trail: 1 = on (the previous frame blended back in half and half), 0 = off.
    uint8_t trail = 0;
    uint8_t driven = 0;

    void reset() { *this = State(); }

    // Nothing to draw, tick or offset: the per-frame paths stop at this check.
    bool idle() const {
        return (fade.value | flash.value | shake.value | letterbox.value | trail) == 0 && !fade.running() &&
               !letterbox.running();
    }

    void tick(int32_t dt12) {
        if (idle()) return;
        fade.tick(dt12);
        flash.tick(dt12);
        shake.tick(dt12);
        letterbox.tick(dt12);
        if (shake.value > 0) shakeStep++;
    }

    // ----- What scripts call (Lua and Blocks) -----
    void fadeOut(uint8_t r, uint8_t g, uint8_t b, int32_t frames) {
        fadeR = r;
        fadeG = g;
        fadeB = b;
        fade.start(255, frames);
    }
    void fadeIn(int32_t frames) { fade.start(0, frames); }
    void startFlash(uint8_t r, uint8_t g, uint8_t b, int32_t frames) {
        flashR = r;
        flashG = g;
        flashB = b;
        flash.set(255);
        flash.start(0, frames);
    }
    void startShake(int32_t amplitude, int32_t frames) {
        if (amplitude < 0) amplitude = 0;
        if (amplitude > 64) amplitude = 64;
        shake.set(amplitude);
        shake.start(0, frames);
    }
    void startLetterbox(int32_t height, int32_t frames, int32_t screenH) {
        letterbox.start(clampBar(height, screenH), frames);
    }
    // Any nonzero value turns the trail on; the blend is always half and half.
    void setTrail(int32_t on) { trail = on != 0 ? 1 : 0; }
    bool isFading() const { return fade.running(); }

    // ----- What cutscene tracks write, every frame they run -----
    void driveFade(int32_t amount) {
        fade.set(amount < 0 ? 0 : amount > 255 ? 255 : amount);
        driven |= kDrivenFade;
    }
    void driveFadeColor(int32_t r, int32_t g, int32_t b) {
        fadeR = clamp8(r);
        fadeG = clamp8(g);
        fadeB = clamp8(b);
        driven |= kDrivenFadeColor;
    }
    void driveLetterbox(int32_t height, int32_t screenH) {
        letterbox.set(clampBar(height, screenH));
        driven |= kDrivenLetterbox;
    }
    void driveShake(int32_t amplitude) {
        shake.set(amplitude < 0 ? 0 : amplitude > 64 ? 64 : amplitude);
        driven |= kDrivenShake;
    }
    void driveTrail(int32_t on) {
        setTrail(on);
        driven |= kDrivenTrail;
    }
    // A stopped cutscene puts back what it drove: no fade, bars, shake or trail.
    void resetDriven() {
        if (driven & kDrivenFade) fade.set(0);
        if (driven & kDrivenFadeColor) fadeR = fadeG = fadeB = 0;
        if (driven & kDrivenLetterbox) letterbox.set(0);
        if (driven & kDrivenShake) shake.set(0);
        if (driven & kDrivenTrail) trail = 0;
        driven = 0;
    }

    // ----- What the renderer draws -----
    void shakeOffsets(int32_t& dx, int32_t& dy) const { shakeOffset(shakeStep, shake.value, dx, dy); }
    // Fade to colour, as GPU passes over the whole screen. The GPU cannot
    // scale what is on screen by an arbitrary factor: it can halve it (50/50
    // blend), subtract a constant and add one. So below half way the fade
    // subtracts a grey of half the amount and adds the colour scaled by it;
    // from half way it first blends the colour in 50/50, then subtracts and
    // adds what is left, reaching exactly the colour at 255. Crossing 128 the
    // picture steps by |p/2 - 64| per channel (p the pixel), about 12 on
    // average over a lit scene; a single subtract all the way is continuous
    // but turns everything but the colour's own channel black by half way.
    struct FadePasses {
        bool half = false;  // 50/50 blend with the colour first
        uint8_t sub = 0;    // then subtract this grey
        uint8_t addR = 0, addG = 0, addB = 0;  // then add this colour
    };
    FadePasses fadePasses() const {
        FadePasses p;
        int32_t a = fade.value < 0 ? 0 : fade.value > 255 ? 255 : fade.value;
        if (a == 0) return p;
        if (a >= 128) {
            p.half = true;
            a = (a - 128) * 255 / 127;
            p.sub = (uint8_t)a;
        } else {
            p.sub = (uint8_t)(a >> 1);
        }
        p.addR = scale255(fadeR, a);
        p.addG = scale255(fadeG, a);
        p.addB = scale255(fadeB, a);
        return p;
    }
    void flashAdd(uint8_t& r, uint8_t& g, uint8_t& b) const {
        r = scale255(flashR, flash.value);
        g = scale255(flashG, flash.value);
        b = scale255(flashB, flash.value);
    }
    // The previous frame is drawn unmodulated (grey 128) at 50/50: a dimmer
    // previous frame would darken the whole picture, not shorten the trail.
    static constexpr uint8_t kTrailGrey = 128;

    static uint8_t clamp8(int32_t v) { return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v); }
    static int32_t clampBar(int32_t h, int32_t screenH) {
        if (h < 0) return 0;
        if (h > screenH / 2) return screenH / 2;
        return h;
    }
};

}  // namespace psxsplash::screenfx
