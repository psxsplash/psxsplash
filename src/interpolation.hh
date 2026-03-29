#pragma once

#include <stdint.h>
#include "cutscene.hh"

namespace psxsplash {

int32_t applyCurve(int32_t t, InterpMode mode);

bool findKfPair(CutsceneKeyframe* kf, uint8_t count, uint16_t frame,
                uint8_t& a, uint8_t& b, int32_t& t, int16_t out[3]);

void lerpKeyframes(CutsceneKeyframe* kf, uint8_t count, uint16_t frame,
                   const int16_t initial[3], int16_t out[3]);

void lerpAngles(CutsceneKeyframe* kf, uint8_t count, uint16_t frame,
                const int16_t initial[3], int16_t out[3]);

}  // namespace psxsplash
