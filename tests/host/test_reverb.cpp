// Host tests for the reverb preset table and switching logic (src/reverb.hh),
// compiled natively: what reverb.cpp programs into the SPU and when.

#include "reverb.hh"

#include "testing.hh"

using namespace psxsplash::reverb;

// Work area sizes and register values from psx-spx "SPU Reverb Examples".
TEST(presets_match_psx_spx) {
    const uint32_t sizes[kPresetCount] = {0, 0x26C0, 0x1F40, 0x4840, 0x6FE0, 0xADE0, 0x3C00, 0xF6C0, 0x18040, 0x18040};
    for (int i = 0; i < kPresetCount; i++) CHECK_EQ(workAreaBytes(i), sizes[i]);
    CHECK_EQ(kPresets[kRoom].regs[0], 0x007D);       // dAPF1
    CHECK_EQ(kPresets[kHall].regs[2], 0x6000);       // vIIR
    CHECK_EQ(kPresets[kSpaceEcho].regs[10], 0x1ED6); // mLSAME
    CHECK_EQ(kPresets[kChaosEcho].regs[7], 0x8100);  // vWALL: the one word Chaos echo and Delay differ in
    CHECK_EQ(kPresets[kDelay].regs[7], 0x0000);
    for (int i = 1; i < kPresetCount; i++) {
        CHECK_EQ(kPresets[i].regs[30], 0x8000);  // vLIN
        CHECK_EQ(kPresets[i].regs[31], 0x8000);  // vRIN
    }
    CHECK(workAreaBytes(kPresetCount) == 0);  // an id from a newer editor is off
}

// ESA = (80000h - size) / 8, and the area ends exactly at the end of sound RAM.
TEST(work_area_base) {
    CHECK_EQ(workAreaBase(kHall), 0xEA44);
    CHECK_EQ(workAreaBase(kChaosEcho), 0xCFF8);
    for (int i = 1; i < kPresetCount; i++) {
        CHECK_EQ(uint32_t(workAreaBase(i)) * 8 + workAreaBytes(i), kSpuRamEnd);
        CHECK_EQ(workAreaBytes(i) % 16, 0u);  // the zero fill is whole ADPCM blocks
        CHECK(workAreaBase(i) > 0x200);        // ESA <= 200h reads as reverb off
    }
}

TEST(fits_above_the_clips) {
    CHECK(fits(kHall, kSpuRamEnd - 0xADE0));
    CHECK(!fits(kHall, kSpuRamEnd - 0xADE0 + 16));
    CHECK(fits(kOff, kSpuRamEnd));
    CHECK(!fits(kPresetCount, 0x1010));
}

// .spu clip pad byte: bit 0 send, bits 4-7 the reservation. 0 (older packs):
// no send, no reservation.
TEST(clip_pad_byte) {
    CHECK(!clipSend(0));
    CHECK_EQ(clipReserve(0), 0);
    CHECK(clipSend(0x51));
    CHECK_EQ(clipReserve(0x51), kHall);
    CHECK(!clipSend(0x90));
    CHECK_EQ(clipReserve(0x90), kDelay);
}

TEST(zone_then_room_then_default) {
    CHECK_EQ(choose(kOff, kOff, kOff), kOff);
    CHECK_EQ(choose(kOff, kOff, kHall), kHall);
    CHECK_EQ(choose(kOff, kRoom, kHall), kRoom);
    CHECK_EQ(choose(kSpaceEcho, kRoom, kHall), kSpaceEcho);
}

TEST(inner_zone_is_smaller) {
    const int32_t u = 4096;
    uint32_t outer = zoneSize(-10 * u, -10 * u, -10 * u, 10 * u, 10 * u, 10 * u);
    uint32_t inner = zoneSize(-2 * u, -2 * u, -2 * u, 2 * u, 2 * u, 2 * u);
    CHECK(inner < outer);
    // Far apart coordinates clamp instead of wrapping.
    uint32_t huge = zoneSize(-0x7FFFFFFF, -0x7FFFFFFF, -0x7FFFFFFF, 0x7FFFFFFF, 0x7FFFFFFF, 0x7FFFFFFF);
    CHECK(huge >= outer);
    CHECK_EQ(zoneSize(5, 5, 5, 0, 0, 0), 0u);
}

// A switch ramps the output down, programs at 0, ramps back up.
TEST(switch_ramps_out_programs_at_zero_ramps_in) {
    Switch s;
    int32_t v;
    CHECK(advance(s, kHall, v));  // from off: programmed at once
    CHECK_EQ(s.active, kHall);
    for (int i = 1; i <= kRampFrames; i++) {
        CHECK(!advance(s, kHall, v));
        CHECK_EQ(v, rampVolume(i));
    }
    CHECK_EQ(v, kOutputVolume);
    CHECK(!advance(s, kHall, v));
    CHECK_EQ(v, -1);  // steady: no writes
    int frames = 0;
    int32_t last = kOutputVolume;
    while (!advance(s, kRoom, v)) {
        CHECK(v < last);
        last = v;
        frames++;
    }
    CHECK_EQ(frames, kRampFrames - 1);
    CHECK_EQ(v, 0);  // output at 0 when the new preset is programmed
    CHECK_EQ(s.active, kRoom);
}

TEST(switch_called_off_half_way_ramps_back) {
    Switch s;
    int32_t v;
    advance(s, kHall, v);
    for (int i = 0; i < kRampFrames; i++) advance(s, kHall, v);
    CHECK(!advance(s, kRoom, v));
    CHECK(!advance(s, kHall, v));  // back before reaching 0: no reprogram
    CHECK_EQ(s.active, kHall);
    CHECK_EQ(v, kOutputVolume);
}

TEST(switching_off_programs_off) {
    Switch s;
    int32_t v;
    advance(s, kHall, v);
    for (int i = 0; i < kRampFrames; i++) advance(s, kHall, v);
    int n = 0;
    while (!advance(s, kOff, v)) n++;
    CHECK_EQ(s.active, kOff);
    CHECK(!advance(s, kOff, v));
    CHECK_EQ(v, -1);  // off stays silent
}

int main() { return psxsplash::test::runAll(); }
