// Host tests for the performance map walk (src/perfmap.hh): the perfmap.pos
// parse and the per-pose state machine. The engine half only pins the player
// and prints what Walk hands it.

#include "perfmap.hh"

#include <stdint.h>
#include <string.h>

#include "testing.hh"

using namespace psxsplash::perfmap;

namespace {

void put16(uint8_t* p, uint16_t v) { p[0] = v & 0xff; p[1] = v >> 8; }
void put32(uint8_t* p, int32_t v) {
    for (int i = 0; i < 4; i++) p[i] = (uint8_t)((uint32_t)v >> (8 * i));
}

// A file with `count` points (i, -i * 4096, 100000 + i), header fields as given.
int makeFile(uint8_t* buf, uint16_t count, uint16_t yaws, uint16_t settle, uint16_t sample) {
    memcpy(buf, "PMAP", 4);
    put16(buf + 4, 1);
    put16(buf + 6, count);
    put16(buf + 8, yaws);
    put16(buf + 10, settle);
    put16(buf + 12, sample);
    put16(buf + 14, 0);
    for (int i = 0; i < count; i++) {
        put32(buf + 16 + i * 12, i);
        put32(buf + 20 + i * 12, -i * 4096);
        put32(buf + 24 + i * 12, 100000 + i);
    }
    return 16 + count * 12;
}

}  // namespace

TEST(parse_reads_header_and_points) {
    uint8_t buf[16 + 3 * 12];
    int n = makeFile(buf, 3, 4, 10, 20);
    Header h;
    CHECK(parse(buf, n, h));
    CHECK_EQ(h.count, 3);
    CHECK_EQ(h.yawCount, 4);
    CHECK_EQ(h.settleFrames, 10);
    CHECK_EQ(h.sampleFrames, 20);
    Point p = pointAt(buf, 2);
    CHECK_EQ(p.x, 2);
    CHECK_EQ(p.y, -8192);
    CHECK_EQ(p.z, 100002);
}

TEST(parse_rejects_bad_files) {
    uint8_t buf[16 + 2 * 12];
    Header h;
    int n = makeFile(buf, 2, 4, 1, 1);
    CHECK(!parse(nullptr, n, h));
    CHECK(!parse(buf, 15, h));             // shorter than the header
    CHECK(!parse(buf, n - 1, h));          // shorter than its points
    buf[0] = 'X';
    CHECK(!parse(buf, n, h));              // magic
    makeFile(buf, 2, 4, 1, 1);
    put16(buf + 4, 2);
    CHECK(!parse(buf, n, h));              // version
    makeFile(buf, 2, 0, 1, 1);
    CHECK(!parse(buf, n, h));              // no yaws
    makeFile(buf, 2, 4, 1, 0);
    CHECK(!parse(buf, n, h));              // no sampled frames
    n = makeFile(buf, 0, 4, 1, 1);
    CHECK(!parse(buf, n, h));              // no points
}

TEST(yaw_raw_is_a_fraction_of_a_full_turn) {
    CHECK_EQ(yawRaw(0, 4), 0);
    CHECK_EQ(yawRaw(1, 4), 512);
    CHECK_EQ(yawRaw(3, 4), 1536);
    CHECK_EQ(yawRaw(1, 3), 682);
}

TEST(sums_saturate_instead_of_wrapping) {
    CHECK_EQ(addSat(1, 2), 3);
    CHECK_EQ(addSat(0xFFFFFFF0u, 0x10), 0xFFFFFFFFu);
    CHECK_EQ(addSat(0xFFFFFFF0u, 0x20), 0xFFFFFFFFu);
}

TEST(parse_rejects_too_many_sample_frames) {
    uint8_t buf[16 + 12];
    Header h;
    int n = makeFile(buf, 1, 1, 1, MAX_SAMPLE_FRAMES);
    CHECK(parse(buf, n, h));
    makeFile(buf, 1, 1, 1, MAX_SAMPLE_FRAMES + 1);
    CHECK(!parse(buf, n, h));
}

TEST(walk_settles_then_averages_and_keeps_the_max) {
    Header h{2, 2, 3, 4};
    Walk w;
    w.begin(h);
    Result r;
    uint32_t sec[SECTIONS] = {};
    int lines = 0, frames = 0;
    // Pose p (0..3) frame f: total 1000*(p+1) + f, render the same minus 100.
    // Settle frames carry a huge reading that must not leak into the average.
    while (w.walking()) {
        int pose = w.point() * 2 + w.yaw();
        int f = frames % 7;
        uint32_t total = f < 3 ? 999999u : 1000u * (pose + 1) + f;
        sec[1] = total - 100;
        bool done = w.frameEnded(sec, total, 100, f == 0 || f == 4 || f == 5, r);
        frames++;
        if (!done) continue;
        CHECK_EQ(r.point, pose / 2);
        CHECK_EQ(r.yaw, pose % 2);
        // samples f = 3..6: mean of 3,4,5,6 is 4 (4.5 floored)
        CHECK_EQ(r.total, 1000u * (pose + 1) + 4);
        CHECK_EQ(r.maxTotal, 1000u * (pose + 1) + 6);
        CHECK_EQ(r.section[1], 1000u * (pose + 1) + 4 - 100);
        CHECK_EQ(r.other, 100);
        CHECK_EQ(r.streamFrames, 2);  // f 4 and 5; the settle frame 0 is not counted
        lines++;
    }
    CHECK_EQ(lines, 4);
    CHECK_EQ(frames, 4 * 7);
    CHECK(!w.frameEnded(sec, 1, 0, false, r));  // finished: ignores further frames
}

TEST(walk_settles_at_least_one_frame) {
    Header h{1, 1, 0, 2};
    Walk w;
    w.begin(h);
    Result r;
    uint32_t sec[SECTIONS] = {};
    CHECK(!w.frameEnded(sec, 999, 0, false, r));  // the frame that still showed the old pose
    CHECK(!w.frameEnded(sec, 10, 0, true, r));
    CHECK(w.frameEnded(sec, 20, 0, false, r));
    CHECK_EQ(r.total, 15);
    CHECK_EQ(r.maxTotal, 20);
    CHECK_EQ(r.streamFrames, 1);
    CHECK(!w.walking());
}

int main() { return psxsplash::test::runAll(); }
