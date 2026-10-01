// Host tests for the streamed-world policy (src/streamplanner.cpp), compiled
// natively. WorldStreamer drives StreamPlanner exactly as these tests do: drop
// everything nextUnload() names, then at most one planLoad() per frame while no
// read is pending.

#include "streamplanner.hh"

#include "testing.hh"

using namespace psxsplash;

namespace {

constexpr int32_t U = 4096;  // one world unit in fp12

// A row of `n` regions along X, each 1 unit wide with 1 unit gaps, each 4 KB.
struct Row {
    SPLASHPACKStreamTable table{};
    SPLASHPACKStreamRegion regions[16]{};
    StreamPlanner planner;

    Row(int n, uint32_t poolBytes, int32_t loadR, int32_t unloadR) {
        table.regionCount = static_cast<uint16_t>(n);
        table.poolBytes = poolBytes;
        table.loadRadius = loadR;
        table.unloadRadius = unloadR;
        for (int i = 0; i < n; i++) {
            regions[i].minX = i * 2 * U;
            regions[i].maxX = i * 2 * U + U;
            regions[i].minZ = 0;
            regions[i].maxZ = U;
            regions[i].byteSize = 4096;
        }
        planner.reset(&table, regions);
    }

    // One frame of WorldStreamer::update, with the read completing instantly.
    int frame(int32_t x, int32_t z) {
        int r;
        while ((r = planner.nextUnload(x, z)) != StreamPlanner::kNone) planner.evict(r);
        uint32_t off;
        uint16_t ev[StreamPlanner::kMaxResident];
        int evc;
        r = planner.planLoad(x, z, off, ev, evc);
        if (r != StreamPlanner::kNone) {
            planner.beginPending(r, off);
            planner.finishPending(true);
        }
        return r;
    }

    void settle(int32_t x, int32_t z) {
        for (int i = 0; i < 64; i++) frame(x, z);
    }

    bool overlapFree() const {
        for (int a = 0; a < planner.residentCount(); a++)
            for (int b = a + 1; b < planner.residentCount(); b++) {
                uint32_t oa = planner.residentOffset(planner.residentRegion(a));
                uint32_t ob = planner.residentOffset(planner.residentRegion(b));
                if (oa < ob + 4096 && ob < oa + 4096) return false;
            }
        return true;
    }
};

}  // namespace

TEST(distance_is_zero_inside_and_squared_outside) {
    SPLASHPACKStreamRegion r{};
    r.minX = 0; r.maxX = U; r.minZ = 0; r.maxZ = U;
    CHECK_EQ(streamRegionDistSq(r, U / 2, U / 2), 0);
    CHECK_EQ(streamRegionDistSq(r, 3 * U, U / 2), static_cast<int64_t>(2 * U) * (2 * U));
    CHECK_EQ(streamRegionDistSq(r, -U, -U), 2 * static_cast<int64_t>(U) * U);
}

TEST(loads_nearest_first_one_per_frame) {
    Row row(4, 4 * 4096, 3 * U, 5 * U);
    // Camera inside region 1: region 1 first, then 0 and 2 (both 1 unit away).
    CHECK_EQ(row.frame(2 * U + U / 2, U / 2), 1);
    int second = row.frame(2 * U + U / 2, U / 2);
    CHECK(second == 0 || second == 2);
    int third = row.frame(2 * U + U / 2, U / 2);
    CHECK(third == 0 || third == 2);
    CHECK(third != second);
    CHECK_EQ(row.frame(2 * U + U / 2, U / 2), StreamPlanner::kNone);  // region 3 is 3.5 units out
    CHECK_EQ(row.planner.residentCount(), 3);
}

TEST(nothing_in_range_loads_nothing) {
    Row row(4, 4 * 4096, U, 2 * U);
    CHECK_EQ(row.frame(100 * U, 100 * U), StreamPlanner::kNone);
    CHECK_EQ(row.planner.residentCount(), 0);
}

TEST(walking_the_row_keeps_only_nearby_regions) {
    Row row(8, 3 * 4096, U + U / 2, 3 * U);
    for (int32_t x = 0; x <= 15 * U; x += U / 4) {
        row.frame(x, U / 2);
        // The region under the camera is never missing for more than a frame.
        int under = x / (2 * U);
        bool inGap = (x % (2 * U)) >= U;
        if (!inGap && under < 8) {
            row.frame(x, U / 2);
            CHECK(row.planner.isResident(under));
        }
        CHECK(row.planner.residentCount() <= 3);
        CHECK(row.overlapFree());
    }
    // At the far end the start of the row is long gone.
    CHECK(!row.planner.isResident(0));
    CHECK(row.planner.isResident(7));
}

TEST(unload_has_hysteresis) {
    Row row(2, 2 * 4096, 2 * U, 4 * U);
    row.settle(U / 2, U / 2);  // inside region 0; region 1 is 1.5 units away -> loaded
    CHECK(row.planner.isResident(1));
    // 3 units from region 1: outside load radius but inside unload radius.
    row.settle(-U, U / 2);
    CHECK(row.planner.isResident(1));
    // 5 units away: dropped.
    row.settle(-3 * U, U / 2);
    CHECK(!row.planner.isResident(1));
}

TEST(full_pool_evicts_only_regions_out_of_load_range) {
    // Pool fits 2 regions; unload radius huge so nothing is dropped by distance.
    Row row(4, 2 * 4096, 2 * U, 1000 * U);
    row.settle(U / 2, U / 2);  // loads 0 and 1
    CHECK(row.planner.isResident(0));
    CHECK(row.planner.isResident(1));
    row.settle(6 * U + U / 2, U / 2);  // inside region 3: needs 2 and 3
    CHECK(row.planner.isResident(3));
    CHECK(row.planner.isResident(2));
    CHECK(!row.planner.isResident(0));
    CHECK(row.overlapFree());
}

TEST(pool_too_small_for_what_is_needed_loads_what_fits) {
    // Camera between regions 0 and 1, both in load range, pool fits one.
    Row row(2, 4096, 2 * U, 4 * U);
    row.settle(U + U / 2, U / 2);
    CHECK_EQ(row.planner.residentCount(), 1);
    CHECK(row.overlapFree());
}

TEST(failed_read_is_retried) {
    Row row(1, 4096, U, 2 * U);
    uint32_t off;
    uint16_t ev[StreamPlanner::kMaxResident];
    int evc;
    int r = row.planner.planLoad(U / 2, U / 2, off, ev, evc);
    CHECK_EQ(r, 0);
    row.planner.beginPending(r, off);
    CHECK_EQ(row.planner.planLoad(U / 2, U / 2, off, ev, evc), StreamPlanner::kNone);  // pending
    row.planner.finishPending(false);
    CHECK(!row.planner.isResident(0));
    CHECK_EQ(row.planner.planLoad(U / 2, U / 2, off, ev, evc), 0);
}

int main() { return psxsplash::test::runAll(); }
