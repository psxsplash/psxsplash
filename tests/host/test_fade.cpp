// Host tests for the fog and edge fade arithmetic (src/fade.hh), compiled
// natively: what renderer.cpp computes per vertex from the GTE's SZ.

#include "fade.hh"

#include "testing.hh"

using namespace psxsplash::fade;

// The renderer drops a triangle once every corner is at or past the ordering
// table size, and culls one whose every corner is past the fog wall. With the
// edge fade on, a vertex there must already be the background colour.
TEST(edge_is_nearest_cull) {
    CHECK_EQ(edgeDepth(8192, 0), 8192);
    CHECK_EQ(edgeDepth(8192, 4000), 4000);
    CHECK_EQ(edgeDepth(3000, 4000), 3000);
    CHECK_EQ(edgeDepth(65536, 0), kMaxSZ);  // SZ saturates below the largest table
    CHECK(kFarPlaneSZ > kMaxSZ);            // the far plane is past every SZ
}

TEST(edge_band_ramps_over_last_eighth) {
    const int32_t e = 8192, start = e - e / 8;
    CHECK_EQ(edgeIR(0, e), 0);
    CHECK_EQ(edgeIR(start, e), 0);
    CHECK_EQ(edgeIR(start + 512, e), 2048);
    CHECK_EQ(edgeIR(e, e), 4096);
    CHECK_EQ(edgeIR(e + 1, e), 4096);
    CHECK_EQ(edgeIR(kMaxSZ, e), 4096);
    CHECK_EQ(edgeIR(e, 0), 0);  // off
}

// No depth jumps by more than one step's worth: the fade is continuous, so
// nothing pops as the camera moves.
TEST(fade_is_monotonic_and_reaches_background_at_every_cull) {
    const uint32_t ots[] = {256, 1000, 2048, 8192, 16384, 65536};
    const int32_t fogs[] = {0, 2000, 4000, 6666, 20000};
    for (uint32_t ot : ots)
        for (int32_t fog : fogs) {
            const int32_t e = edgeDepth(ot, fog);
            int32_t prev = 0, worst = 0;
            for (int32_t sz = 0; sz <= kMaxSZ; sz++) {
                int32_t ir = vertexIR(sz, fog, e);
                CHECK(ir >= prev);
                if (ir - prev > worst) worst = ir - prev;
                prev = ir;
            }
            // Over E/8 depth steps, the largest single step is about 4096 / (E/8).
            CHECK(worst <= 4096 / (e / 8) + 1);
            // At the table end and the fog wall: background.
            if (ot <= (uint32_t)kMaxSZ) CHECK_EQ(vertexIR((int32_t)ot, fog, e), 4096);
            if (fog > 0) CHECK_EQ(vertexIR(fog + 1, fog, e), 4096);
        }
}

TEST(fog_only_matches_old_ramp) {
    const int32_t f = 4000;
    for (int32_t sz = 0; sz < 5000; sz += 7) {
        int32_t fogNear = f >> 3, want;
        if (sz <= fogNear) want = 0;
        else if (sz >= f) want = 4096;
        else want = ((sz - fogNear) * 4096) / (f - fogNear);
        CHECK_EQ(vertexIR(sz, f, 0), want);
    }
}

TEST(fog_wall_inside_table_is_unchanged) {
    // E is the fog wall, and the fog ramp is already above the edge band.
    const int32_t f = 4000, e = edgeDepth(16384, f);
    int bad = 0;
    for (int32_t sz = 0; sz <= f + 10; sz++)
        if (vertexIR(sz, f, e) != vertexIR(sz, f, 0)) bad++;
    CHECK_EQ(bad, 0);
}

int main() { return psxsplash::test::runAll(); }
