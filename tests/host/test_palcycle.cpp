// Host tests for palette cycling (src/palettecycle.hh): the pack record's validation and the step
// and rotation arithmetic PaletteCycler runs every frame.

#include <stdint.h>
#include <string.h>

#include "palettecycle.hh"

#include "testing.hh"

using namespace psxsplash::palcycle;

namespace {

struct Pack {
    alignas(4) uint8_t bytes[512];
    uint32_t size = 0;
};

// A pack with 8 filler bytes, then a record for a CLUT of clutLength words (base[i] = 0x100 + i).
Pack makePack(uint16_t clutLength, const ClutCycleRange* ranges, uint8_t n, uint32_t at = 8) {
    Pack p;
    memset(p.bytes, 0xee, sizeof(p.bytes));
    ClutCycleHeader h{uint16_t((clutLength + 1) & ~1), n, 0};
    memcpy(p.bytes + at, &h, sizeof(h));
    for (unsigned i = 0; i < h.length; i++) {
        uint16_t w = uint16_t(0x100 + i);
        memcpy(p.bytes + at + 4 + i * 2, &w, 2);
    }
    memcpy(p.bytes + at + 4 + h.length * 2, ranges, n * sizeof(ClutCycleRange));
    p.size = at + recordSize(h.length, n);
    return p;
}

RecordError parse(const Pack& p, uint16_t clutLength, Record& r, uint32_t at = 8, uint16_t pad = kClutHasCycles) {
    return parseRecord(p.bytes, p.size, at, pad, clutLength, r);
}

// What the CLUT shows at a step, entry by entry, straight from the definition.
uint16_t naive(const uint16_t* base, const ClutCycleRange& r, uint16_t step, unsigned entry) {
    if (entry < r.first || entry >= unsigned(r.first) + r.count) return base[entry];
    unsigned i = entry - r.first;
    int s = r.direction > 0 ? step : -int(step);
    int j = ((int(i) + s) % int(r.count) + int(r.count)) % int(r.count);
    return base[r.first + j];
}

}  // namespace

TEST(record_layout) {
    CHECK_EQ(sizeof(ClutCycleHeader), 4);
    CHECK_EQ(sizeof(ClutCycleRange), 6);
    CHECK_EQ(recordSize(20, 1), 4 + 40 + 6);
    CHECK_EQ(recordSize(256, 8), 4 + 512 + 48);
}

TEST(record_parses) {
    ClutCycleRange rs[2] = {{0, 4, 256, 1, 0}, {10, 7, 512, -1, 0}};
    Pack p = makePack(20, rs, 2);
    Record r;
    CHECK_EQ(int(parse(p, 20, r)), int(RecordError::Ok));
    CHECK_EQ(r.length, 20);
    CHECK_EQ(r.rangeCount, 2);
    CHECK_EQ(r.base[0], 0x100);
    CHECK_EQ(r.base[19], 0x113);
    CHECK_EQ(r.ranges[1].first, 10);
    CHECK_EQ(r.ranges[1].count, 7);
    CHECK_EQ(r.ranges[1].period, 512);
    CHECK_EQ(r.ranges[1].direction, -1);
    // An odd CLUT length: the record carries it rounded up.
    Pack q = makePack(15, rs, 1);
    CHECK_EQ(int(parse(q, 15, r)), int(RecordError::Ok));
    CHECK_EQ(r.length, 16);
}

TEST(record_absent_is_not_an_error) {
    ClutCycleRange rs[1] = {{0, 4, 256, 1, 0}};
    Pack p = makePack(20, rs, 1);
    Record r;
    CHECK_EQ(int(parse(p, 20, r, 8, 0)), int(RecordError::NoRecord));  // pad bit clear: stale offset ignored
    CHECK_EQ(int(parse(p, 20, r, 0)), int(RecordError::NoRecord));     // offset 0
}

TEST(record_rejects_bad_placement) {
    ClutCycleRange rs[1] = {{0, 4, 256, 1, 0}};
    Pack p = makePack(20, rs, 1);
    Record r;
    CHECK_EQ(int(parse(p, 20, r, 6)), int(RecordError::Misaligned));
    CHECK_EQ(int(parseRecord(p.bytes, p.size, (p.size + 3) & ~3u, 1, 20, r)), int(RecordError::OutOfBounds));
    CHECK_EQ(int(parseRecord(p.bytes, p.size, 1000, 1, 20, r)), int(RecordError::OutOfBounds));
    CHECK_EQ(int(parseRecord(p.bytes, p.size - 1, 8, 1, 20, r)), int(RecordError::OutOfBounds));  // ranges cut
    CHECK_EQ(int(parseRecord(p.bytes, 10, 8, 1, 20, r)), int(RecordError::OutOfBounds));          // header cut
}

TEST(record_rejects_bad_contents) {
    Record r;
    {
        ClutCycleRange rs[1] = {{0, 4, 256, 1, 0}};
        Pack p = makePack(20, rs, 1);
        CHECK_EQ(int(parse(p, 22, r)), int(RecordError::BadLength));  // length is not this CLUT's
        uint16_t odd = 19;
        memcpy(p.bytes + 8, &odd, 2);
        CHECK_EQ(int(parse(p, 19, r)), int(RecordError::BadLength));
    }
    {
        ClutCycleRange rs[1] = {{0, 4, 256, 1, 0}};
        Pack p = makePack(20, rs, 0);
        CHECK_EQ(int(parse(p, 20, r)), int(RecordError::BadRangeCount));
        ClutCycleRange many[9];
        for (int i = 0; i < 9; i++) many[i] = {uint8_t(i * 2), 2, 256, 1, 0};
        Pack q = makePack(20, many, 9);
        CHECK_EQ(int(parse(q, 20, r)), int(RecordError::BadRangeCount));
    }
    struct Case {
        ClutCycleRange range;
        RecordError want;
    } cases[] = {
        {{17, 4, 256, 1, 0}, RecordError::RangeOutsideClut},  // 17 + 4 > 20
        {{16, 4, 256, 1, 0}, RecordError::Ok},                // ends exactly at the CLUT end
        {{3, 1, 256, 1, 0}, RecordError::CountTooSmall},
        {{3, 0, 256, 1, 0}, RecordError::CountTooSmall},
        {{0, 4, 0, 1, 0}, RecordError::ZeroPeriod},
        {{0, 4, 256, 0, 0}, RecordError::BadDirection},
        {{0, 4, 256, 2, 0}, RecordError::BadDirection},
        {{250, 10, 256, 1, 0}, RecordError::RangeOutsideClut},  // first + count past 255 does not wrap
    };
    for (auto& c : cases) {
        Pack p = makePack(20, &c.range, 1);
        CHECK_EQ(int(parse(p, 20, r)), int(c.want));
    }
    // A 15-word CLUT: its record is 16 long, but the 16th word is not the CLUT's.
    ClutCycleRange pastOdd = {12, 4, 256, 1, 0};
    Pack o = makePack(15, &pastOdd, 1);
    CHECK_EQ(int(parse(o, 15, r)), int(RecordError::RangeOutsideClut));
}

TEST(record_rejects_overlap) {
    Record r;
    ClutCycleRange overlap[2] = {{0, 6, 256, 1, 0}, {5, 4, 256, 1, 0}};
    Pack p = makePack(20, overlap, 2);
    CHECK_EQ(int(parse(p, 20, r)), int(RecordError::Overlap));
    ClutCycleRange touching[2] = {{0, 5, 256, 1, 0}, {5, 4, 256, 1, 0}};
    Pack q = makePack(20, touching, 2);
    CHECK_EQ(int(parse(q, 20, r)), int(RecordError::Ok));
}

// The period is in units of dt12 / 16 = 1/7680 s, so a frame at 30 fps (dt12 4096) and two at
// 60 fps (dt12 2048 each) advance the same.
TEST(step_time_is_rate_independent) {
    const uint16_t period = 7680 / 4;  // a quarter second per step
    uint32_t acc30 = 0, acc60 = 0;
    uint16_t s30 = 0, s60 = 0;
    for (int f = 0; f < 30 * 3; f++) advance(acc30, s30, 4096, period, 18);
    for (int f = 0; f < 60 * 3; f++) advance(acc60, s60, 2048, period, 18);
    CHECK_EQ(s30, 12);  // 3 s at 4 steps per second
    CHECK_EQ(s60, 12);
    CHECK_EQ(acc30, acc60);
}

TEST(step_advances_and_reports_changes) {
    uint32_t acc = 0;
    uint16_t step = 0;
    const uint16_t period = 512;  // 8192 dt12: every second 30 fps frame
    CHECK(!advance(acc, step, 4096, period, 5));
    CHECK(advance(acc, step, 4096, period, 5));
    CHECK_EQ(step, 1);
    CHECK_EQ(acc, 0);
    // A long frame moves several steps at once, modulo the count.
    CHECK(advance(acc, step, 8192 * 6, period, 5));
    CHECK_EQ(step, 2);  // 1 + 6 = 7 = 2 mod 5
    // A whole number of cycles leaves the CLUT as it was: no upload.
    CHECK(!advance(acc, step, 8192 * 5, period, 5));
    CHECK_EQ(step, 2);
    CHECK(!advance(acc, step, 0, period, 5));
    CHECK(!advance(acc, step, -4096, period, 5));
    // Short periods: several steps per frame.
    acc = 0;
    step = 0;
    CHECK(advance(acc, step, 4096, 1, 18));  // 256 steps = 4 mod 18
    CHECK_EQ(step, 256 % 18);
    // The longest period: 65535 units, about 8.5 s, does not overflow.
    acc = 0;
    step = 0;
    int changes = 0;
    for (int f = 0; f < 30 * 9; f++) changes += advance(acc, step, 4096, 65535, 2);
    CHECK_EQ(changes, 1);
}

TEST(rotation_direction) {
    CHECK_EQ(rotationFor(0, 1, 18), 0);
    CHECK_EQ(rotationFor(3, 1, 18), 3);
    CHECK_EQ(rotationFor(0, -1, 18), 0);
    CHECK_EQ(rotationFor(3, -1, 18), 15);
    CHECK_EQ(rotationFor(21, 1, 18), 3);
}

TEST(rotate_two_copies_matches_definition) {
    uint16_t src[37], dst[37];
    for (int i = 0; i < 37; i++) src[i] = uint16_t(1000 + i);
    const unsigned counts[] = {2, 3, 16, 37};
    for (unsigned count : counts)
        for (unsigned k = 0; k < count * 2; k++) {
            rotateRange(dst, src, count, k);
            bool ok = true;
            for (unsigned i = 0; i < count; i++) ok &= dst[i] == src[(i + k) % count];
            CHECK(ok);
        }
}

TEST(upload_span_is_even_and_covers_every_range) {
    ClutCycleRange a[1] = {{0, 18, 1, 1, 0}};
    Span s = uploadSpan(a, 1, 20);
    CHECK_EQ(s.start, 0);
    CHECK_EQ(s.end, 18);
    ClutCycleRange b[1] = {{3, 4, 1, 1, 0}};  // entries 3..6
    s = uploadSpan(b, 1, 20);
    CHECK_EQ(s.start, 2);
    CHECK_EQ(s.end, 8);
    ClutCycleRange c[2] = {{9, 2, 1, 1, 0}, {1, 3, 1, 1, 0}};  // 9..10 and 1..3
    s = uploadSpan(c, 2, 16);
    CHECK_EQ(s.start, 0);
    CHECK_EQ(s.end, 12);
    ClutCycleRange d[1] = {{13, 2, 1, 1, 0}};  // 13..14 of a 15-word CLUT, record 16 long
    s = uploadSpan(d, 1, 16);
    CHECK_EQ(s.start, 12);
    CHECK_EQ(s.end, 16);
}

TEST(build_span_matches_definition) {
    ClutCycleRange rs[2] = {{1, 5, 256, 1, 0}, {9, 6, 256, -1, 0}};
    Pack p = makePack(16, rs, 2);
    Record r;
    CHECK_EQ(int(parse(p, 16, r)), int(RecordError::Ok));
    Span span = uploadSpan(r.ranges, r.rangeCount, r.length);
    if (!CHECK_EQ(span.start, 0) || !CHECK_EQ(span.end, 16)) return;  // the loop below indexes from 0
    for (uint16_t s0 = 0; s0 < 7; s0++)
        for (uint16_t s1 = 0; s1 < 8; s1++) {
            uint16_t steps[2] = {uint16_t(s0 % 5), uint16_t(s1 % 6)};
            uint16_t out[16];
            buildSpan(out, r, steps, span);
            bool ok = true;
            for (unsigned e = 0; e < 16; e++) {
                uint16_t want = r.base[e];
                if (e >= 1 && e < 6) want = naive(r.base, rs[0], steps[0], e);
                if (e >= 9 && e < 15) want = naive(r.base, rs[1], steps[1], e);
                ok &= out[e - span.start] == want;
            }
            CHECK(ok);
        }
    // A span that starts mid-CLUT writes from its own start.
    ClutCycleRange one[1] = {{3, 4, 256, 1, 0}};
    Pack q = makePack(20, one, 1);
    CHECK_EQ(int(parse(q, 20, r)), int(RecordError::Ok));
    span = uploadSpan(r.ranges, 1, r.length);
    uint16_t steps[1] = {1};
    uint16_t out[6];
    buildSpan(out, r, steps, span);
    const uint16_t want[6] = {0x102, 0x104, 0x105, 0x106, 0x103, 0x107};
    CHECK_MEM(out, want, sizeof(want));
}

// The stage-A probe's prediction for the courtyard stone CLUT: entries 0..17 at step 3.
TEST(courtyard_prediction) {
    const uint16_t stone[20] = {0x18c6, 0x318d, 0x35ae, 0x35ce, 0x39f0, 0x31ad, 0x35cf, 0x2d8d, 0x2d8c, 0x39ef,
                                0x31ae, 0x39cf, 0x3e10, 0x3df0, 0x3e11, 0x2d6c, 0x296c, 0x296b, 0x0000, 0x0000};
    Record r;
    ClutCycleRange range = {0, 18, 1, 1, 0};
    r.base = stone;
    r.ranges = &range;
    r.length = 20;
    r.rangeCount = 1;
    uint16_t steps[1] = {3}, out[18];
    buildSpan(out, r, steps, uploadSpan(&range, 1, 20));
    const uint16_t readback[18] = {0x35ce, 0x39f0, 0x31ad, 0x35cf, 0x2d8d, 0x2d8c, 0x39ef, 0x31ae, 0x39cf,
                                   0x3e10, 0x3df0, 0x3e11, 0x2d6c, 0x296c, 0x296b, 0x18c6, 0x318d, 0x35ae};
    CHECK_MEM(out, readback, sizeof(readback));
}

// The packet the GPU gets: GP0(A0h), the span's rectangle, the words, and GP0(01h) last. Real
// hardware keeps drawing with its cached CLUT after an upload unless the cache is flushed
// (measured on an SCPH-5501; palettecycle.hh), so the flush is part of the format, not a nicety.
TEST(upload_packet_ends_with_cache_flush) {
    ClutCycleRange rs[1] = {{3, 4, 256, 1, 0}};
    Pack p = makePack(20, rs, 1);
    Record r;
    CHECK_EQ(int(parse(p, 20, r)), int(RecordError::Ok));
    Span span = uploadSpan(r.ranges, 1, r.length);  // entries 2..7
    CHECK_EQ(uploadPacketWords(span), 3 + 3 + 1);
    uint32_t out[16];
    for (uint32_t& w : out) w = 0xdeadbeef;
    uint16_t steps[1] = {1};
    const unsigned n = buildUploadPacket(out, r, steps, span, 32, 241);
    CHECK_EQ(n, 7);
    CHECK_EQ(out[0], kGp0VramUpload);
    CHECK_EQ(out[1], (241u << 16) | (32 + 2));  // y, x of the span's first word
    CHECK_EQ(out[2], (1u << 16) | 6);           // 1 high, 6 wide
    CHECK_EQ(out[3], 0x0102u | (0x0104u << 16));
    CHECK_EQ(out[4], 0x0105u | (0x0106u << 16));
    CHECK_EQ(out[5], 0x0103u | (0x0107u << 16));
    CHECK_EQ(out[n - 1], kGp0FlushCache);
    CHECK_EQ(kGp0FlushCache, 0x01000000u);
    CHECK_EQ(out[n], 0xdeadbeefu);  // nothing past the packet
    // A whole 64-entry CLUT: 32 words between header and flush.
    ClutCycleRange all[1] = {{0, 64, 256, 1, 0}};
    Pack q = makePack(64, all, 1);
    CHECK_EQ(int(parse(q, 64, r)), int(RecordError::Ok));
    span = uploadSpan(r.ranges, 1, r.length);
    uint32_t big[40];
    uint16_t st[1] = {0};
    CHECK_EQ(buildUploadPacket(big, r, st, span, 0, 240), 3 + 32 + 1);
    CHECK_EQ(big[3 + 32], kGp0FlushCache);
}

int main() { return psxsplash::test::runAll(); }
