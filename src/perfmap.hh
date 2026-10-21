#pragma once

// Performance map walk: the pure half (file parse and the per-pose state
// machine), kept free of psyqo so tests/host can build it natively. The engine
// half is in scenemanager.cpp, under PSXSPLASH_PROFILER and PCDRV_SUPPORT.
//
// A profiler dev build reads "perfmap.pos" beside the scene at scene start.
// When it is there, the engine walks the player through every point and yaw
// and prints one "psxsplash: perf point ..." line per pose, which the editor
// paints on the nav mesh. Absent, nothing changes.
//
// Output (stdout in Redux, psxmon's text relay on hardware), gpu.now() units:
//   psxsplash: perf begin <count> <yawCount>
//   psxsplash: perf point I yaw K total T max M render R anim A coll C lua L
//              ctrl P nav N other O stream S          (one line, per pose)
//   psxsplash: perf done <count>
// T and the sections are averages over sampleFrames, M the largest total,
// S how many sampled frames had streaming I/O queued or in flight. Instead of
// begin: "perf noplayer" (no nav regions, so no player) or "perf badfile <size>";
// "perf abort I" when a script changes scene mid-walk.
//
// perfmap.pos, little-endian, 16-byte header then the points:
//   char[4] "PMAP"
//   u16 version         1
//   u16 count           points, at least 1
//   u16 yawCount        yaws per point, at least 1; yaw k is k*360/yawCount
//                       degrees, 0 = the player's rotation.y of 0
//   u16 settleFrames    frames at a pose before sampling (at least 1 is used:
//                       the first frame at a pose still renders the old one)
//   u16 sampleFrames    frames averaged per pose, 1..1024
//   u16 pad             0
//   count x { i32 x, i32 y, i32 z }   20.12 world units, the raw numbers
//                       Player.SetPosition(x, y, z) takes. y is the player's
//                       EYE (camera) height, the feet are y + playerHeight
//                       (PS1 Y points down). Used verbatim, never snapped to
//                       the nav floor: the editor passes floorY - playerHeight.

#include <stdint.h>

namespace psxsplash::perfmap {

static constexpr uint16_t VERSION = 1;
static constexpr int HEADER_SIZE = 16;
static constexpr int POINT_SIZE = 12;
static constexpr uint16_t MAX_SAMPLE_FRAMES = 1024;

struct Header {
    uint16_t count = 0;
    uint16_t yawCount = 0;
    uint16_t settleFrames = 0;
    uint16_t sampleFrames = 0;
};

struct Point {
    int32_t x, y, z;
};

inline uint16_t readU16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
inline int32_t readI32(const uint8_t* p) {
    return (int32_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24));
}

// Validates a perfmap.pos image. Returns false (and leaves `out` alone) for a
// wrong magic or version, a zero count, yawCount or sampleFrames, or a file
// shorter than its points.
inline bool parse(const uint8_t* data, int size, Header& out) {
    if (data == nullptr || size < HEADER_SIZE) return false;
    if (data[0] != 'P' || data[1] != 'M' || data[2] != 'A' || data[3] != 'P') return false;
    if (readU16(data + 4) != VERSION) return false;
    Header h;
    h.count = readU16(data + 6);
    h.yawCount = readU16(data + 8);
    h.settleFrames = readU16(data + 10);
    h.sampleFrames = readU16(data + 12);
    if (h.count == 0 || h.yawCount == 0 || h.sampleFrames == 0) return false;
    if (h.sampleFrames > MAX_SAMPLE_FRAMES) return false;
    if (size < HEADER_SIZE + (int)h.count * POINT_SIZE) return false;
    out = h;
    return true;
}

inline Point pointAt(const uint8_t* data, uint16_t i) {
    const uint8_t* p = data + HEADER_SIZE + (int)i * POINT_SIZE;
    return {readI32(p), readI32(p + 4), readI32(p + 8)};
}

// 32-bit sums: the engine links -nostdlib, without libgcc's 64-bit helpers.
// MAX_SAMPLE_FRAMES frames of up to 4 s each fit; past that a sum saturates.
inline uint32_t addSat(uint32_t a, uint32_t b) { return a > 0xFFFFFFFFu - b ? 0xFFFFFFFFu : a + b; }

// psyqo::Angle raw value of yaw k (2048 = a full turn).
inline int32_t yawRaw(uint16_t k, uint16_t yawCount) { return (int32_t)((uint32_t)k * 2048u / yawCount); }

static constexpr int SECTIONS = 6;  // the profiler's sections, in its enum order

// One finished pose: the sampled frames' averages and the largest total.
struct Result {
    uint16_t point, yaw;
    uint32_t total, maxTotal, other;
    uint32_t section[SECTIONS];
    uint16_t streamFrames;  // sampled frames with streaming I/O queued or in flight
};

// Pose order: point 0 yaw 0, point 0 yaw 1, ..., point 1 yaw 0, ...
class Walk {
  public:
    void begin(const Header& h) {
        m_h = h;
        if (m_h.settleFrames < 1) m_h.settleFrames = 1;
        m_point = m_yaw = 0;
        m_frame = 0;
        m_walking = true;
        clear();
    }
    void stop() { m_walking = false; }
    bool walking() const { return m_walking; }
    uint16_t point() const { return m_point; }
    uint16_t yaw() const { return m_yaw; }
    const Header& header() const { return m_h; }

    // Call once per frame after the profiler closed it, with that frame's
    // readings; `streaming`: streaming I/O was queued or in flight. True when the frame finished a pose; `r` then holds it and
    // point()/yaw() already name the next pose (or walking() is false).
    bool frameEnded(const uint32_t (&section)[SECTIONS], uint32_t total, uint32_t other, bool streaming, Result& r) {
        if (!m_walking) return false;
        uint32_t f = m_frame++;
        if (f < m_h.settleFrames) return false;
        for (int i = 0; i < SECTIONS; i++) m_sum[i] = addSat(m_sum[i], section[i]);
        m_sumTotal = addSat(m_sumTotal, total);
        m_sumOther = addSat(m_sumOther, other);
        if (total > m_max) m_max = total;
        if (streaming) m_streamFrames++;
        if (m_frame < (uint32_t)m_h.settleFrames + m_h.sampleFrames) return false;

        uint32_t n = m_h.sampleFrames;
        r.point = m_point;
        r.yaw = m_yaw;
        r.total = m_sumTotal / n;
        r.maxTotal = m_max;
        r.streamFrames = m_streamFrames;
        r.other = m_sumOther / n;
        for (int i = 0; i < SECTIONS; i++) r.section[i] = m_sum[i] / n;
        clear();
        m_frame = 0;
        if (++m_yaw == m_h.yawCount) {
            m_yaw = 0;
            if (++m_point == m_h.count) m_walking = false;
        }
        return true;
    }

  private:
    void clear() {
        for (auto& s : m_sum) s = 0;
        m_sumTotal = m_sumOther = 0;
        m_max = 0;
        m_streamFrames = 0;
    }

    Header m_h;
    uint16_t m_point = 0, m_yaw = 0;
    uint32_t m_frame = 0;
    bool m_walking = false;
    uint32_t m_sum[SECTIONS] = {};
    uint32_t m_sumTotal = 0, m_sumOther = 0;
    uint32_t m_max = 0;
    uint16_t m_streamFrames = 0;
};

}  // namespace psxsplash::perfmap
