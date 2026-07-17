#include "spritemath.hh"

namespace psxsplash {
namespace spritemath {

AnimTick advanceAnim(uint8_t frame, uint8_t timer, uint8_t frameCount, uint8_t frameDuration,
                     bool loop) {
    if (frameCount == 0) return {0, 0, true};
    // A zero duration is exporter nonsense; treat it as "advance every frame"
    // rather than freezing the animation forever on frame 0.
    if (frameDuration == 0) frameDuration = 1;

    timer++;
    if (timer < frameDuration) return {frame, timer, false};

    if (frame + 1 < frameCount) return {(uint8_t)(frame + 1), 0, false};
    if (loop) return {0, 0, false};
    return {frame, 0, true};  // hold the last frame
}

void frameToUV(const SpriteSheet& sheet, uint8_t frame, uint8_t& u, uint8_t& v) {
    const uint8_t cols = sheet.cols ? sheet.cols : 1;
    u = (uint8_t)(sheet.u0 + (frame % cols) * sheet.cellW);
    v = (uint8_t)(sheet.v0 + (frame / cols) * sheet.cellH);
}

uint8_t facingFromYaw(int32_t yawRaw, uint8_t dirCount) {
    if (dirCount == 0) return 0;
    const int32_t sector = c_angleFullTurn / dirCount;
    int32_t a = yawRaw + sector / 2;  // round to nearest sector, not down
    a %= c_angleFullTurn;
    if (a < 0) a += c_angleFullTurn;  // C's % keeps the dividend's sign
    return (uint8_t)((a * dirCount) / c_angleFullTurn);
}

}  // namespace spritemath
}  // namespace psxsplash
