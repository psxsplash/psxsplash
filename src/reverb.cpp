#include "reverb.hh"

#include <psyqo/xprintf.h>

#include "common/hardware/counters.h"
#include "common/hardware/spu.h"
#include <psyqo/spu.hh>

volatile uint32_t psxsplash_reverbStallHblanks = 0;

namespace psxsplash {

namespace {

constexpr uint16_t kAttrReverbMaster = 1 << 7;
constexpr uint16_t kAttrCdReverb = 1 << 2;

// DMA source for the zero fill: 1 KB of .bss, sent as often as the work area
// needs.
uint32_t s_zero[256];

void writeRegs(const uint16_t* regs) {
    volatile uint16_t* r = reinterpret_cast<volatile uint16_t*>(SPU_REVERB);
    for (int i = 0; i < 32; i++) r[i] = regs[i];
}

void setOutput(int16_t v) {
    SPU_REVERB_LEFT = static_cast<uint16_t>(v);
    SPU_REVERB_RIGHT = static_cast<uint16_t>(v);
}

uint32_t hblanks() {
    uint32_t v;
    do { v = COUNTERS[1].value; } while (v != COUNTERS[1].value);
    return v;
}

}  // namespace

void ReverbSystem::reset() {
    SPU_CTRL &= ~(kAttrReverbMaster | kAttrCdReverb);
    setOutput(0);
    writeRegs(reverb::kOffRegs);
    m_zoneCount = 0;
    m_default = reverb::kOff;
    m_clipsEnd = reverb::kSpuRamEnd;
    m_switch = reverb::Switch{};
}

void ReverbSystem::begin(uint8_t sceneDefault, bool cdReverb, uint32_t clipsEnd) {
    m_default = reverb::valid(sceneDefault) ? sceneDefault : reverb::kOff;
    m_clipsEnd = clipsEnd;
    if (cdReverb) SPU_CTRL |= kAttrCdReverb;
    else SPU_CTRL &= ~kAttrCdReverb;
}

void ReverbSystem::addZone(int32_t minX, int32_t minY, int32_t minZ,
                           int32_t maxX, int32_t maxY, int32_t maxZ, uint8_t preset) {
    if (preset == reverb::kOff || !reverb::valid(preset) || m_zoneCount >= kMaxZones) return;
    Zone& z = m_zones[m_zoneCount++];
    z.minX = minX; z.minY = minY; z.minZ = minZ;
    z.maxX = maxX; z.maxY = maxY; z.maxZ = maxZ;
    z.size = reverb::zoneSize(minX, minY, minZ, maxX, maxY, maxZ);
    z.preset = preset;
}

uint8_t ReverbSystem::zoneAt(int32_t minX, int32_t minY, int32_t minZ,
                             int32_t maxX, int32_t maxY, int32_t maxZ) const {
    uint8_t best = reverb::kOff;
    uint32_t bestSize = 0xFFFFFFFFu;
    for (int i = 0; i < m_zoneCount; i++) {
        const Zone& z = m_zones[i];
        // The test detectTriggers uses, so a zone switches with its onTriggerEnter.
        if (minX > z.maxX || maxX < z.minX || minY > z.maxY || maxY < z.minY ||
            minZ > z.maxZ || maxZ < z.minZ) continue;
        if (z.size < bestSize) { bestSize = z.size; best = z.preset; }
    }
    return best;
}

void ReverbSystem::start(uint8_t zonePreset, uint8_t roomPreset) {
    uint8_t wanted = reverb::choose(zonePreset, roomPreset, m_default);
    if (!reverb::fits(wanted, m_clipsEnd)) wanted = reverb::kOff;
    if (wanted == reverb::kOff) return;
    program(wanted);
    m_switch.active = wanted;
    m_switch.step = reverb::kRampFrames;
    setOutput(reverb::rampVolume(m_switch.step));
    printf("psxsplash: reverb %u %s\n", unsigned(wanted), reverb::name(wanted));
}

void ReverbSystem::update(uint8_t zonePreset, uint8_t roomPreset) {
    uint8_t wanted = reverb::choose(zonePreset, roomPreset, m_default);
    if (!reverb::fits(wanted, m_clipsEnd)) wanted = reverb::kOff;

    int32_t volume;
    bool now = reverb::advance(m_switch, wanted, volume);
    if (volume >= 0) setOutput(static_cast<int16_t>(volume));
    if (!now) return;
    program(wanted);
    printf("psxsplash: reverb %u %s\n", unsigned(wanted), reverb::name(wanted));
}

// Output is already at 0. psx-spx: stop the SPU writing to the buffer (ATTR
// bit 7), load the preset, point ESA at its work area, zero-fill it, enable.
void ReverbSystem::program(uint8_t id) {
    SPU_CTRL &= ~kAttrReverbMaster;
    setOutput(0);
    if (id == reverb::kOff) {
        writeRegs(reverb::kOffRegs);
        return;
    }
    writeRegs(reverb::kPresets[id].regs);
    SPU_REVERB_ADDR = reverb::workAreaBase(id);

    uint32_t start = hblanks();
    uint32_t addr = reverb::kSpuRamEnd - reverb::workAreaBytes(id);
    uint32_t left = reverb::workAreaBytes(id);
    while (left > 0) {
        uint32_t n = left > sizeof(s_zero) ? sizeof(s_zero) : left;
        psyqo::SPU::dmaWrite(addr, s_zero, n);
        addr += n;
        left -= n;
    }
    SPU_CTRL &= ~(0b11 << 4);
    psxsplash_reverbStallHblanks = (hblanks() - start) & 0xFFFF;

    SPU_CTRL |= kAttrReverbMaster;
}

}  // namespace psxsplash
