#pragma once

#include <stdint.h>

// SPU hardware reverb: the preset table, the work area each preset needs at the
// top of sound RAM, and the choice of which preset is active. Free of psyqo so
// tests/host/test_reverb.cpp compiles this exact code natively; the register
// writes are in reverb.cpp.
//
// Where the ids come from (all were padding the editor wrote 0, so a pack older
// than the feature reads as "no reverb" everywhere):
//   scene default   header renderFlags bits 1-4, CD-music reverb bit 7
//   room            RoomData::reverbPreset, 0 = the scene default
//   trigger zone    SPLASHPACKTriggerBox::reverbPreset, 0 = not a reverb zone
//   clip            .spu clip pad byte: bit 0 = the clip's voices feed the
//                   reverb (EON), bits 4-7 = the largest preset the scene uses,
//                   read before the clips are uploaded to reserve its work area

namespace psxsplash::reverb {

enum : uint8_t {
    kOff = 0,
    kRoom = 1,
    kStudioSmall = 2,
    kStudioMedium = 3,
    kStudioLarge = 4,
    kHall = 5,
    kHalfEcho = 6,
    kSpaceEcho = 7,
    kChaosEcho = 8,
    kDelay = 9,
    kPresetCount = 10,
};

constexpr uint32_t kSpuRamEnd = 0x80000;

struct Preset {
    const char* name;
    uint32_t workAreaBytes;  // the buffer runs from 0x80000 - this to the end of sound RAM
    // 1F801DC0h..1F801DFEh in register order (dAPF1 .. vRIN), from psx-spx
    // "SPU Reverb Examples".
    uint16_t regs[32];
};

inline constexpr Preset kPresets[kPresetCount] = {
    {"off", 0, {}},
    {"room", 0x26C0, {
        0x007D, 0x005B, 0x6D80, 0x54B8, 0xBED0, 0x0000, 0x0000, 0xBA80,
        0x5800, 0x5300, 0x04D6, 0x0333, 0x03F0, 0x0227, 0x0374, 0x01EF,
        0x0334, 0x01B5, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
        0x0000, 0x0000, 0x01B4, 0x0136, 0x00B8, 0x005C, 0x8000, 0x8000}},
    {"studio small", 0x1F40, {
        0x0033, 0x0025, 0x70F0, 0x4FA8, 0xBCE0, 0x4410, 0xC0F0, 0x9C00,
        0x5280, 0x4EC0, 0x03E4, 0x031B, 0x03A4, 0x02AF, 0x0372, 0x0266,
        0x031C, 0x025D, 0x025C, 0x018E, 0x022F, 0x0135, 0x01D2, 0x00B7,
        0x018F, 0x00B5, 0x00B4, 0x0080, 0x004C, 0x0026, 0x8000, 0x8000}},
    {"studio medium", 0x4840, {
        0x00B1, 0x007F, 0x70F0, 0x4FA8, 0xBCE0, 0x4510, 0xBEF0, 0xB4C0,
        0x5280, 0x4EC0, 0x0904, 0x076B, 0x0824, 0x065F, 0x07A2, 0x0616,
        0x076C, 0x05ED, 0x05EC, 0x042E, 0x050F, 0x0305, 0x0462, 0x02B7,
        0x042F, 0x0265, 0x0264, 0x01B2, 0x0100, 0x0080, 0x8000, 0x8000}},
    {"studio large", 0x6FE0, {
        0x00E3, 0x00A9, 0x6F60, 0x4FA8, 0xBCE0, 0x4510, 0xBEF0, 0xA680,
        0x5680, 0x52C0, 0x0DFB, 0x0B58, 0x0D09, 0x0A3C, 0x0BD9, 0x0973,
        0x0B59, 0x08DA, 0x08D9, 0x05E9, 0x07EC, 0x04B0, 0x06EF, 0x03D2,
        0x05EA, 0x031D, 0x031C, 0x0238, 0x0154, 0x00AA, 0x8000, 0x8000}},
    {"hall", 0xADE0, {
        0x01A5, 0x0139, 0x6000, 0x5000, 0x4C00, 0xB800, 0xBC00, 0xC000,
        0x6000, 0x5C00, 0x15BA, 0x11BB, 0x14C2, 0x10BD, 0x11BC, 0x0DC1,
        0x11C0, 0x0DC3, 0x0DC0, 0x09C1, 0x0BC4, 0x07C1, 0x0A00, 0x06CD,
        0x09C2, 0x05C1, 0x05C0, 0x041A, 0x0274, 0x013A, 0x8000, 0x8000}},
    {"half echo", 0x3C00, {
        0x0017, 0x0013, 0x70F0, 0x4FA8, 0xBCE0, 0x4510, 0xBEF0, 0x8500,
        0x5F80, 0x54C0, 0x0371, 0x02AF, 0x02E5, 0x01DF, 0x02B0, 0x01D7,
        0x0358, 0x026A, 0x01D6, 0x011E, 0x012D, 0x00B1, 0x011F, 0x0059,
        0x01A0, 0x00E3, 0x0058, 0x0040, 0x0028, 0x0014, 0x8000, 0x8000}},
    {"space echo", 0xF6C0, {
        0x033D, 0x0231, 0x7E00, 0x5000, 0xB400, 0xB000, 0x4C00, 0xB000,
        0x6000, 0x5400, 0x1ED6, 0x1A31, 0x1D14, 0x183B, 0x1BC2, 0x16B2,
        0x1A32, 0x15EF, 0x15EE, 0x1055, 0x1334, 0x0F2D, 0x11F6, 0x0C5D,
        0x1056, 0x0AE1, 0x0AE0, 0x07A2, 0x0464, 0x0232, 0x8000, 0x8000}},
    {"chaos echo", 0x18040, {
        0x0001, 0x0001, 0x7FFF, 0x7FFF, 0x0000, 0x0000, 0x0000, 0x8100,
        0x0000, 0x0000, 0x1FFF, 0x0FFF, 0x1005, 0x0005, 0x0000, 0x0000,
        0x1005, 0x0005, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
        0x0000, 0x0000, 0x1004, 0x1002, 0x0004, 0x0002, 0x8000, 0x8000}},
    {"delay", 0x18040, {
        0x0001, 0x0001, 0x7FFF, 0x7FFF, 0x0000, 0x0000, 0x0000, 0x0000,
        0x0000, 0x0000, 0x1FFF, 0x0FFF, 0x1005, 0x0005, 0x0000, 0x0000,
        0x1005, 0x0005, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
        0x0000, 0x0000, 0x1004, 0x1002, 0x0004, 0x0002, 0x8000, 0x8000}},
};

// psx-spx "Reverb off": the memory offsets are 1, not 0, or zero-filling the
// buffer is unreliable.
inline constexpr uint16_t kOffRegs[32] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1,
    0, 0, 1, 1, 1, 1, 1, 1, 0, 0, 1, 1, 1, 1, 0, 0};

inline bool valid(uint8_t id) { return id < kPresetCount; }
inline uint32_t workAreaBytes(uint8_t id) { return valid(id) ? kPresets[id].workAreaBytes : 0; }
inline const char* name(uint8_t id) { return valid(id) ? kPresets[id].name : "off"; }
// ESA (1F801DA2h): the work area start in 8-byte units.
inline uint16_t workAreaBase(uint8_t id) {
    return static_cast<uint16_t>((kSpuRamEnd - workAreaBytes(id)) >> 3);
}

// A preset fits when its work area starts at or above the end of the clips.
inline bool fits(uint8_t id, uint32_t clipsEnd) {
    return valid(id) && workAreaBytes(id) <= kSpuRamEnd - clipsEnd;
}

// .spu clip pad byte.
inline bool clipSend(uint8_t pad) { return (pad & 1) != 0; }
inline uint8_t clipReserve(uint8_t pad) { return pad >> 4; }

// Innermost zone first, then the room, then the scene default. 0 means "none"
// for a zone and "inherit" for a room.
inline uint8_t choose(uint8_t zonePreset, uint8_t roomPreset, uint8_t sceneDefault) {
    if (zonePreset != kOff) return zonePreset;
    if (roomPreset != kOff) return roomPreset;
    return sceneDefault;
}

// Size of a zone, to pick the innermost of overlapping ones: extents in 1/16
// units (raw 20.12 >> 8), clamped so the sum of their pairwise products fits
// 32 bits. No 64-bit arithmetic: the engine links without libgcc.
inline uint32_t zoneSize(int32_t minX, int32_t minY, int32_t minZ,
                         int32_t maxX, int32_t maxY, int32_t maxZ) {
    auto ext = [](int32_t lo, int32_t hi) -> uint32_t {
        uint32_t e = hi > lo ? (static_cast<uint32_t>(hi) - static_cast<uint32_t>(lo)) >> 8 : 0;
        return e > 0x7FFF ? 0x7FFF : e;
    };
    uint32_t x = ext(minX, maxX), y = ext(minY, maxY), z = ext(minZ, maxZ);
    return x * y + y * z + x * z;
}

// Output volume ramp around a switch, so the old tail and the zero fill are
// not heard as a click.
constexpr int kRampFrames = 4;
constexpr int16_t kOutputVolume = 0x3FFF;
inline int16_t rampVolume(int step) {  // step 0 .. kRampFrames
    if (step <= 0) return 0;
    if (step >= kRampFrames) return kOutputVolume;
    return static_cast<int16_t>((kOutputVolume * step) / kRampFrames);
}

// One frame of a switch: the output ramps down to 0 over kRampFrames, the
// new preset is programmed at 0 (advance returns true; active is already the
// new id), then it ramps back up. A switch called off half way ramps back in.
// volume: what to write to the output volume this frame, -1 for nothing.
struct Switch {
    uint8_t active = kOff;
    int step = 0;  // 0 .. kRampFrames
};
inline bool advance(Switch& s, uint8_t wanted, int32_t& volume) {
    volume = -1;
    if (wanted == s.active) {
        if (s.active != kOff && s.step < kRampFrames) volume = rampVolume(++s.step);
        return false;
    }
    if (s.step > 0) {
        volume = rampVolume(--s.step);
        if (s.step > 0) return false;
    }
    s.active = wanted;
    return true;
}

}  // namespace psxsplash::reverb

namespace psxsplash {

// The SPU side: owns ATTR bit 7, the reverb registers, the work area and the
// output volume. SceneManager feeds it the zone and room presets every frame.
class ReverbSystem {
  public:
    static constexpr int kMaxZones = 32;

    // Reverb off, every register at psx-spx's "Reverb off" values, no zones.
    void reset();
    // After a scene loaded: its default preset, whether CD music feeds the
    // reverb, and the end of the clips in sound RAM (a preset whose work area
    // would overlap them is treated as off).
    void begin(uint8_t sceneDefault, bool cdReverb, uint32_t clipsEnd);
    void addZone(int32_t minX, int32_t minY, int32_t minZ,
                 int32_t maxX, int32_t maxY, int32_t maxZ, uint8_t preset);
    // The preset of the innermost zone the box touches, 0 when none.
    uint8_t zoneAt(int32_t minX, int32_t minY, int32_t minZ,
                   int32_t maxX, int32_t maxY, int32_t maxZ) const;
    // During scene load, before any Lua runs: programs the starting preset at
    // full volume, so a voice keyed at scene creation already feeds it (Redux
    // only routes a voice to the reverb if it is on when the voice is keyed).
    void start(uint8_t zonePreset, uint8_t roomPreset);
    // Once per frame.
    void update(uint8_t zonePreset, uint8_t roomPreset);
    uint8_t active() const { return m_switch.active; }

  private:
    void program(uint8_t id);

    struct Zone {
        int32_t minX, minY, minZ, maxX, maxY, maxZ;
        uint32_t size;
        uint8_t preset;
    };
    Zone m_zones[kMaxZones];
    int m_zoneCount = 0;
    uint8_t m_default = 0;
    uint32_t m_clipsEnd = reverb::kSpuRamEnd;
    reverb::Switch m_switch;
};

}  // namespace psxsplash

// Sound RAM zero fill of the last switch, in hblanks (64 us each): the frame
// it lands in stalls by this much. Read by the Redux oracle.
extern "C" volatile uint32_t psxsplash_reverbStallHblanks;
