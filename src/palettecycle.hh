#pragma once

// Palette cycling: the pack record and the pure arithmetic PaletteCycler runs on it. No psyqo
// dependency, so tests/host compiles this header natively.
//
// Pack format (no version bump). In the main splashpack, a SPLASHPACKClut whose pad has
// kClutHasCycles set and whose clutOffset is nonzero points at a resident, 4-byte aligned record:
//
//   ClutCycleHeader              length (even, = the CLUT length rounded up to even), rangeCount
//   uint16_t base[length]        the CLUT's words as uploaded at load (step 0 of every range)
//   ClutCycleRange[rangeCount]   the ranges, which must not overlap
//
// Engines older than this skip the CLUT table (and with it both fields), so a pack with cycles
// still boots there, with the static palette.
//
// Timing: a range advances one step every `period` units of 1/7680 s, which is dt12 / 16 (dt12 is
// 4096 per 1/30 s whatever the refresh rate, so 30 and 60 fps cycle at the same speed). One unit
// is 0.13 ms; the longest step is 65535 units, 8.53 s.
//
// Direction +1: at step s, entry first + i shows base[first + (i + s) % count], so colours move
// one entry towards the start of the range per step. Direction -1 moves them the other way.

#include <stdint.h>

namespace psxsplash::palcycle {

constexpr uint16_t kClutHasCycles = 1;  // SPLASHPACKClut::pad bit 0
constexpr int kPeriodShift = 4;         // period unit = dt12 >> kPeriodShift = 1/7680 s
constexpr int kMaxRanges = 8;           // per CLUT

struct ClutCycleHeader {
    uint16_t length;
    uint8_t rangeCount;
    uint8_t pad;
};
static_assert(sizeof(ClutCycleHeader) == 4, "ClutCycleHeader must be 4 bytes");

struct ClutCycleRange {
    uint8_t first;
    uint8_t count;
    uint16_t period;
    int8_t direction;
    uint8_t pad;
};
static_assert(sizeof(ClutCycleRange) == 6, "ClutCycleRange must be 6 bytes");
static_assert(__builtin_offsetof(ClutCycleRange, period) == 2, "ClutCycleRange.period at 2");
static_assert(__builtin_offsetof(ClutCycleRange, direction) == 4, "ClutCycleRange.direction at 4");

enum class RecordError : uint8_t {
    Ok,
    NoRecord,        // pad bit clear or offset 0: not an error, the CLUT does not cycle
    OutOfBounds,     // header, words or ranges past the end of the pack
    Misaligned,      // offset not 4-byte aligned
    BadLength,       // odd, or not the CLUT length rounded up to even
    BadRangeCount,   // 0 or more than kMaxRanges
    RangeOutsideClut,
    CountTooSmall,   // count < 2
    ZeroPeriod,
    BadDirection,    // not +1 or -1
    Overlap,
};

inline const char* recordErrorName(RecordError e) {
    switch (e) {
        case RecordError::Ok: return "ok";
        case RecordError::NoRecord: return "no record";
        case RecordError::OutOfBounds: return "out of bounds";
        case RecordError::Misaligned: return "misaligned";
        case RecordError::BadLength: return "bad length";
        case RecordError::BadRangeCount: return "bad range count";
        case RecordError::RangeOutsideClut: return "range outside the CLUT";
        case RecordError::CountTooSmall: return "range count below 2";
        case RecordError::ZeroPeriod: return "zero period";
        case RecordError::BadDirection: return "bad direction";
        case RecordError::Overlap: return "overlapping ranges";
    }
    return "?";
}

struct Record {
    const uint16_t* base = nullptr;
    const ClutCycleRange* ranges = nullptr;
    uint16_t length = 0;
    uint8_t rangeCount = 0;
};

inline uint32_t recordSize(uint16_t length, uint8_t rangeCount) {
    return sizeof(ClutCycleHeader) + uint32_t(length) * 2 + uint32_t(rangeCount) * sizeof(ClutCycleRange);
}

// Validates the record a CLUT table entry points at. clutLength is the entry's own length field.
inline RecordError parseRecord(const uint8_t* pack, uint32_t packSize, uint32_t offset, uint16_t pad,
                               uint16_t clutLength, Record& out) {
    if (!(pad & kClutHasCycles) || offset == 0) return RecordError::NoRecord;
    if (offset & 3) return RecordError::Misaligned;
    if (offset >= packSize || packSize - offset < sizeof(ClutCycleHeader)) return RecordError::OutOfBounds;
    ClutCycleHeader h;
    __builtin_memcpy(&h, pack + offset, sizeof(h));
    if ((h.length & 1) || h.length == 0 || h.length != ((clutLength + 1) & ~1)) return RecordError::BadLength;
    if (h.rangeCount == 0 || h.rangeCount > kMaxRanges) return RecordError::BadRangeCount;
    if (packSize - offset < recordSize(h.length, h.rangeCount)) return RecordError::OutOfBounds;
    const auto* ranges = reinterpret_cast<const ClutCycleRange*>(pack + offset + sizeof(h) + h.length * 2u);
    for (unsigned i = 0; i < h.rangeCount; i++) {
        const ClutCycleRange& r = ranges[i];
        if (r.count < 2) return RecordError::CountTooSmall;
        if (unsigned(r.first) + r.count > clutLength) return RecordError::RangeOutsideClut;
        if (r.period == 0) return RecordError::ZeroPeriod;
        if (r.direction != 1 && r.direction != -1) return RecordError::BadDirection;
        for (unsigned j = 0; j < i; j++) {
            const ClutCycleRange& o = ranges[j];
            if (r.first < o.first + o.count && o.first < r.first + r.count) return RecordError::Overlap;
        }
    }
    out.base = reinterpret_cast<const uint16_t*>(pack + offset + sizeof(h));
    out.ranges = ranges;
    out.length = h.length;
    out.rangeCount = h.rangeCount;
    return RecordError::Ok;
}

// Adds dt12 to a range's accumulator; returns true when its step changed.
inline bool advance(uint32_t& acc, uint16_t& step, int32_t dt12, uint16_t period, uint8_t count) {
    if (dt12 <= 0 || period == 0 || count < 2) return false;
    const uint32_t p = uint32_t(period) << kPeriodShift;
    acc += uint32_t(dt12);
    if (acc < p) return false;
    const uint32_t n = acc / p;
    acc -= n * p;
    const uint16_t moved = uint16_t(n % count);
    if (moved == 0) return false;
    step = uint16_t((step + moved) % count);
    return true;
}

// The rotation applied at a step: dst[i] = src[(i + k) % count].
inline unsigned rotationFor(uint16_t step, int8_t direction, uint8_t count) {
    step %= count;
    return direction < 0 && step ? count - step : step;
}

// dst[i] = src[(i + k) % count] for i in [0, count), as two contiguous copies.
inline void rotateRange(uint16_t* dst, const uint16_t* src, unsigned count, unsigned k) {
    k %= count;
    __builtin_memcpy(dst, src + k, (count - k) * 2);
    __builtin_memcpy(dst + (count - k), src, k * 2);
}

// The words a CLUT's upload covers: the union of its ranges, widened to even start and end.
struct Span {
    uint16_t start, end;
};
inline Span uploadSpan(const ClutCycleRange* ranges, uint8_t rangeCount, uint16_t length) {
    unsigned lo = length, hi = 0;
    for (unsigned i = 0; i < rangeCount; i++) {
        if (ranges[i].first < lo) lo = ranges[i].first;
        if (unsigned(ranges[i].first) + ranges[i].count > hi) hi = ranges[i].first + ranges[i].count;
    }
    lo &= ~1u;
    hi = (hi + 1) & ~1u;
    if (hi > length) hi = length;
    return {uint16_t(lo), uint16_t(hi)};
}

// Fills out[0 .. span.end - span.start) with the CLUT words of the span at the given steps.
inline void buildSpan(uint16_t* out, const Record& rec, const uint16_t* steps, Span span) {
    __builtin_memcpy(out, rec.base + span.start, (span.end - span.start) * 2);
    for (unsigned i = 0; i < rec.rangeCount; i++) {
        const ClutCycleRange& r = rec.ranges[i];
        rotateRange(out + (r.first - span.start), rec.base + r.first, r.count,
                    rotationFor(steps[i], r.direction, r.count));
    }
}

// The GP0 packet of one CLUT's upload: GP0(A0h), its rectangle, the span's words, then GP0(01h).
//
// The trailing GP0(01h) is required on hardware. Measured on a retail SCPH-5501 with a test that
// draws a sprite with CLUT C, uploads new words into C in the same command chain, then draws with C
// again and reads both back: without GP0(01h) the second draw used the OLD words at 4 and 8 bpp
// (all 256 texels, every run), because the GPU's CLUT cache only reloads when a draw uses a
// different CLUT than the last one; with GP0(01h), or with a draw on another CLUT in between, it
// used the new words. psx-spx says the same ("the CLUT cache is invalidated when different CLUT
// index values are used or when GP0(01h) is issued"). PCSX-Redux does not model the CLUT cache and
// shows the new words either way, so only hardware catches a missing flush.
constexpr uint32_t kGp0VramUpload = 0xa0000000;
constexpr uint32_t kGp0FlushCache = 0x01000000;

inline unsigned uploadPacketWords(Span span) { return 3 + (span.end - span.start) / 2u + 1; }

// Writes uploadPacketWords(span) words at out (4-byte aligned) for the CLUT at (clutX, clutY).
inline unsigned buildUploadPacket(uint32_t* out, const Record& rec, const uint16_t* steps, Span span, int16_t clutX,
                                  int16_t clutY) {
    const unsigned width = span.end - span.start;
    out[0] = kGp0VramUpload;
    out[1] = (uint32_t(uint16_t(clutY)) << 16) | uint16_t(clutX + span.start);
    out[2] = (1u << 16) | width;
    buildSpan(reinterpret_cast<uint16_t*>(out + 3), rec, steps, span);
    out[3 + width / 2] = kGp0FlushCache;
    return 3 + width / 2 + 1;
}

}  // namespace psxsplash::palcycle
