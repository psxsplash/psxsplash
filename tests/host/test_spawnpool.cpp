// Host tests for spawn pools: the v33 header size, the table validation and the
// bookkeeping behind Scene.Spawn, Scene.Despawn and Scene.PoolCount
// (src/spawnpool.cpp), compiled natively. The Lua side is covered by the boot
// test (tests/boot/fixture-spawnpool).

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "spawnpool.hh"
#include "splashpackversion.hh"

#include "testing.hh"

using namespace psxsplash;

namespace {

// A tiny pack: the table at offset 0 and the names after it, so nameOffset is
// a plain byte offset from the start of the buffer, as in a splashpack.
struct Pack {
    uint8_t bytes[256];
    uint32_t used = 0;
    uint16_t poolCount = 0;

    Pack() { memset(bytes, 0, sizeof(bytes)); used = 4 + 8 * 8; }

    void put16(uint32_t at, uint16_t v) { bytes[at] = uint8_t(v); bytes[at + 1] = uint8_t(v >> 8); }
    void put32(uint32_t at, uint32_t v) { put16(at, uint16_t(v)); put16(at + 2, uint16_t(v >> 16)); }

    void add(uint16_t first, uint16_t count, const char* name) {
        const uint32_t nameAt = used;
        memcpy(bytes + used, name, strlen(name) + 1);
        used += uint32_t(strlen(name)) + 1;
        const uint32_t rec = 4 + 8 * poolCount;
        put16(rec, first);
        put16(rec + 2, count);
        put32(rec + 4, nameAt);
        poolCount++;
        put16(0, poolCount);
    }
};

// The "is object i active" source the engine reads from GameObject flags.
struct World {
    bool active[32];
    World() { memset(active, 0, sizeof(active)); }
    auto reader() const {
        return [this](uint32_t i) { return active[i]; };
    }
};

}  // namespace

TEST(header_size_v33) {
    CHECK_EQ(splashpackHeaderSize(32), 172);
    CHECK_EQ(splashpackHeaderSize(33), 176);
    CHECK_EQ(splashpackHeaderSize(40), 176);
    CHECK_EQ(kSplashpackHeaderSizeV33 - kSplashpackHeaderSizeV32, 4);
    CHECK(splashpackVersionSupported(33));
    CHECK_EQ(kSplashpackNewestVersion, 33);
    CHECK_EQ(splashpackHeaderSize(kSplashpackNewestVersion), kSplashpackHeaderSizeV33);
}

TEST(find_by_name) {
    Pack p;
    p.add(2, 3, "bullets");
    p.add(6, 2, "sparks");
    CHECK(spawnpool::validateTable(p.bytes, 10) == nullptr);
    SpawnPools pools;
    pools.load(p.bytes, p.bytes);
    CHECK_EQ(pools.poolCount(), 2);
    CHECK_EQ(pools.find("bullets"), 0);
    CHECK_EQ(pools.find("sparks"), 1);
    CHECK_EQ(pools.first(1), 6);
    CHECK_EQ(pools.capacity(1), 2);
    CHECK(strcmp(pools.name(0), "bullets") == 0);
    pools.unload();
}

TEST(unknown_name) {
    Pack p;
    p.add(0, 2, "bullets");
    SpawnPools pools;
    pools.load(p.bytes, p.bytes);
    CHECK_EQ(pools.find("bullet"), SpawnPools::NONE);
    CHECK_EQ(pools.find("bullets2"), SpawnPools::NONE);
    CHECK_EQ(pools.find(""), SpawnPools::NONE);
    CHECK_EQ(pools.find(nullptr), SpawnPools::NONE);
    pools.unload();
    // No pools at all: a scene without the table.
    SpawnPools none;
    none.load(p.bytes, nullptr);
    CHECK_EQ(none.poolCount(), 0);
    CHECK_EQ(none.find("bullets"), SpawnPools::NONE);
}

TEST(spawn_order_lowest_free_first) {
    Pack p;
    p.add(4, 4, "pool");
    SpawnPools pools;
    pools.load(p.bytes, p.bytes);
    World w;
    for (int expect = 4; expect < 8; expect++) {
        const int got = pools.pickFree(0, w.reader());
        CHECK_EQ(got, expect);
        w.active[got] = true;
    }
    pools.unload();
}

TEST(spawn_ignores_objects_outside_the_run) {
    Pack p;
    p.add(4, 2, "pool");
    SpawnPools pools;
    pools.load(p.bytes, p.bytes);
    World w;
    // Everything around the run is active or not, it must not matter.
    w.active[3] = w.active[6] = true;
    CHECK_EQ(pools.pickFree(0, w.reader()), 4);
    w.active[4] = true;
    CHECK_EQ(pools.pickFree(0, w.reader()), 5);
    w.active[5] = true;
    CHECK_EQ(pools.pickFree(0, w.reader()), -1);
    pools.unload();
}

TEST(exhaustion_returns_none) {
    Pack p;
    p.add(0, 3, "pool");
    SpawnPools pools;
    pools.load(p.bytes, p.bytes);
    World w;
    w.active[0] = w.active[1] = w.active[2] = true;
    CHECK_EQ(pools.pickFree(0, w.reader()), -1);
    CHECK_EQ(pools.activeCount(0, w.reader()), 3);
    pools.unload();
}

TEST(despawn_frees_the_slot) {
    Pack p;
    p.add(0, 3, "pool");
    SpawnPools pools;
    pools.load(p.bytes, p.bytes);
    World w;
    w.active[0] = w.active[1] = w.active[2] = true;
    CHECK_EQ(pools.pickFree(0, w.reader()), -1);
    w.active[1] = false;  // Scene.Despawn of the middle copy
    CHECK_EQ(pools.pickFree(0, w.reader()), 1);
    w.active[0] = false;
    CHECK_EQ(pools.pickFree(0, w.reader()), 0);  // lowest again, not the one freed last
    pools.unload();
}

TEST(despawn_of_a_non_member) {
    Pack p;
    p.add(2, 2, "a");
    p.add(5, 1, "b");
    SpawnPools pools;
    pools.load(p.bytes, p.bytes);
    CHECK_EQ(pools.poolOf(0), SpawnPools::NONE);
    CHECK_EQ(pools.poolOf(1), SpawnPools::NONE);
    CHECK_EQ(pools.poolOf(2), 0);
    CHECK_EQ(pools.poolOf(3), 0);
    CHECK_EQ(pools.poolOf(4), SpawnPools::NONE);
    CHECK_EQ(pools.poolOf(5), 1);
    CHECK_EQ(pools.poolOf(6), SpawnPools::NONE);
    CHECK_EQ(pools.poolOf(0xFFFF), SpawnPools::NONE);
    CHECK_EQ(pools.poolOf(0xFFFFFFFFu), SpawnPools::NONE);
    pools.unload();
}

TEST(counts) {
    Pack p;
    p.add(1, 4, "a");
    p.add(5, 2, "b");
    SpawnPools pools;
    pools.load(p.bytes, p.bytes);
    World w;
    CHECK_EQ(pools.activeCount(0, w.reader()), 0);
    w.active[0] = true;  // outside both runs
    w.active[1] = w.active[3] = true;
    w.active[5] = true;
    CHECK_EQ(pools.activeCount(0, w.reader()), 2);
    CHECK_EQ(pools.capacity(0), 4);
    CHECK_EQ(pools.activeCount(1, w.reader()), 1);
    CHECK_EQ(pools.capacity(1), 2);
    pools.unload();
}

TEST(validate_accepts_a_sound_table) {
    Pack p;
    p.add(0, 2, "a");
    p.add(2, 2, "b");   // touching runs do not overlap
    p.add(4, 1, "c");
    CHECK(spawnpool::validateTable(p.bytes, 5) == nullptr);
    Pack none;
    CHECK(spawnpool::validateTable(none.bytes, 0) == nullptr);  // poolCount 0
}

TEST(validate_rejects_a_run_past_the_objects) {
    Pack p;
    p.add(3, 3, "a");
    CHECK(spawnpool::validateTable(p.bytes, 5) != nullptr);
    CHECK(spawnpool::validateTable(p.bytes, 6) == nullptr);
    Pack q;
    q.add(0xFFFF, 2, "a");  // first + count wraps a uint16
    CHECK(spawnpool::validateTable(q.bytes, 0xFFFF) != nullptr);
    Pack r;
    r.add(7, 1, "a");
    CHECK(spawnpool::validateTable(r.bytes, 3) != nullptr);
}

TEST(validate_rejects_overlap) {
    Pack p;
    p.add(0, 4, "a");
    p.add(3, 3, "b");  // shares object 3
    CHECK(spawnpool::validateTable(p.bytes, 10) != nullptr);
    Pack q;
    q.add(4, 4, "a");
    q.add(0, 5, "b");  // earlier-indexed run reaches into the first
    CHECK(spawnpool::validateTable(q.bytes, 10) != nullptr);
    Pack r;
    r.add(2, 6, "a");
    r.add(4, 1, "b");  // inside
    CHECK(spawnpool::validateTable(r.bytes, 10) != nullptr);
    Pack s;
    s.add(2, 2, "a");
    s.add(2, 2, "b");  // identical
    CHECK(spawnpool::validateTable(s.bytes, 10) != nullptr);
}

TEST(validate_rejects_empty_and_nameless) {
    Pack p;
    p.add(1, 0, "a");
    CHECK(spawnpool::validateTable(p.bytes, 10) != nullptr);
    Pack q;
    q.add(1, 2, "a");
    q.put32(4 + 4, 0);  // nameOffset 0
    CHECK(spawnpool::validateTable(q.bytes, 10) != nullptr);
    Pack r;
    r.add(0, 1, "a");
    r.put16(0, MAX_SPAWN_POOLS + 1);
    CHECK(spawnpool::validateTable(r.bytes, 10) != nullptr);
    CHECK(spawnpool::validateTable(nullptr, 10) != nullptr);
}

int main() { return ::psxsplash::test::runAll(); }
