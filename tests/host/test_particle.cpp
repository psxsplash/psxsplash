// Host tests for particle emitters: the v31 header size, the table parse, the
// pool size and the tick-by-tick simulation (src/particlemath.hh), compiled
// natively. The golden values at the end are what SplashEdit's editor tests
// (testParticles in splashedit-ng) assert of its own copy of the simulation,
// so the Scene view preview and the console stay in step.

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "particlemath.hh"
#include "splashpackversion.hh"

#include "testing.hh"

using namespace psxsplash;
using namespace psxsplash::particlemath;

TEST(header_size_v31) {
    CHECK_EQ(splashpackHeaderSize(28), 160);
    CHECK_EQ(splashpackHeaderSize(30), 160);
    CHECK_EQ(splashpackHeaderSize(31), 172);
    CHECK_EQ(splashpackHeaderSize(40), 172);
    // Two reserved words, then the particle table offset.
    CHECK_EQ(kSplashpackHeaderSizeV31 - kSplashpackHeaderSizeV28, 12);
}

TEST(record_layout) {
    CHECK_EQ(sizeof(ParticleEmitterRecord), 76);
    CHECK_EQ(offsetof(ParticleEmitterRecord, pos), 4);
    CHECK_EQ(offsetof(ParticleEmitterRecord, vel), 16);
    CHECK_EQ(offsetof(ParticleEmitterRecord, spread), 28);
    CHECK_EQ(offsetof(ParticleEmitterRecord, gravity), 40);
    CHECK_EQ(offsetof(ParticleEmitterRecord, rate), 44);
    CHECK_EQ(offsetof(ParticleEmitterRecord, lifetime), 48);
    CHECK_EQ(offsetof(ParticleEmitterRecord, maxParticles), 50);
    CHECK_EQ(offsetof(ParticleEmitterRecord, burst), 52);
    CHECK_EQ(offsetof(ParticleEmitterRecord, duration), 54);
    CHECK_EQ(offsetof(ParticleEmitterRecord, gameObject), 56);
    CHECK_EQ(offsetof(ParticleEmitterRecord, size), 58);
    CHECK_EQ(offsetof(ParticleEmitterRecord, sheet), 60);
    CHECK_EQ(offsetof(ParticleEmitterRecord, flags), 63);
    CHECK_EQ(offsetof(ParticleEmitterRecord, startColor), 64);
    CHECK_EQ(offsetof(ParticleEmitterRecord, endColor), 67);
    CHECK_EQ(offsetof(ParticleEmitterRecord, seed), 72);
    CHECK_EQ(sizeof(Particle), 28);
}

TEST(pool_bytes) {
    CHECK_EQ(poolBytes(0, 0), 0u);
    CHECK_EQ(poolBytes(1, 64), 36u + 64u * 28u);
    CHECK_EQ(poolBytes(3, 300), 3u * 36u + 300u * 28u);
}

TEST(table_parse) {
    alignas(4) uint8_t t[4 + 2 * sizeof(ParticleEmitterRecord)] = {};
    t[0] = 2;
    ParticleEmitterRecord* r = reinterpret_cast<ParticleEmitterRecord*>(t + 4);
    r[0].maxParticles = 10;
    r[1].maxParticles = 20;
    const ParticleEmitterRecord* out[4];
    CHECK_EQ(parseTable(t, out, 4), 2);
    CHECK_EQ(out[1]->maxParticles, 20);
    CHECK_EQ(parseTable(t, out, 1), 1);
}

namespace {
ParticleEmitterRecord fountain() {
    ParticleEmitterRecord r = {};
    r.vel[0] = 0;
    r.vel[1] = -4096 * 2;  // up 2 units a tick
    r.vel[2] = 0;
    r.spread[0] = 4096;
    r.spread[2] = 4096;
    r.gravity = 256;
    r.rate = 65536 / 2;  // one particle every other tick
    r.lifetime = 30;
    r.maxParticles = 8;
    r.burst = 3;
    r.duration = 40;
    r.flags = PARTICLE_LOOP;
    r.seed = 12345;
    return r;
}
}  // namespace

TEST(lcg_and_spread) {
    CHECK_EQ(lcgNext(0), 1013904223u);
    CHECK_EQ(lcgNext(1), 1015568748u);
    CHECK_EQ(spreadOf(0x00000000u, 4096), -4096);
    CHECK_EQ(spreadOf(0x80000000u, 4096), 0);
    CHECK_EQ(spreadOf(0xFFFFFFFFu, 4096), 4095);
}

TEST(burst_rate_and_ring) {
    ParticleEmitterRecord r = fountain();
    Particle pool[8];
    ParticleEmitterState e = {};
    e.rec = &r;
    e.particles = pool;
    e.origin[0] = 100;
    e.origin[1] = -50;
    e.origin[2] = 7;
    play(e);
    CHECK_EQ(e.count, 3);
    CHECK_EQ(pool[0].pos[0], 100 * 4096);
    CHECK_EQ(pool[0].pos[1], -50 * 4096);
    tick(e);  // accum 0.5
    CHECK_EQ(e.count, 3);
    tick(e);  // accum 1.0 -> one spawn
    CHECK_EQ(e.count, 4);
    // Ages: the burst is 2 ticks old, the new one 0.
    CHECK_EQ(pool[0].age, 2);
    CHECK_EQ(pool[3].age, 0);
    // Position after 2 ticks: y += vel, then vel += gravity: -2u, then -2u + 256.
    CHECK_EQ(pool[0].pos[1], -50 * 4096 - 8192 - (8192 - 256));
    CHECK_EQ(pool[0].vel[1], -8192 + 512);
    // Fill: the pool holds 8; more spawns are dropped.
    burst(e, 10);
    CHECK_EQ(e.count, 8);
    // Retire: after the lifetime the burst goes first, ring head moves.
    for (int i = 0; i < 28; i++) tick(e);
    CHECK_EQ(e.head, 3);
    stop(e);
    for (int i = 0; i < 40; i++) tick(e);
    CHECK_EQ(e.count, 0);
    CHECK(!e.playing);
}

TEST(one_shot_stops_after_duration) {
    ParticleEmitterRecord r = fountain();
    r.flags = 0;
    r.duration = 4;
    r.rate = 65536;
    Particle pool[8];
    ParticleEmitterState e = {};
    e.rec = &r;
    e.particles = pool;
    play(e);
    for (int i = 0; i < 4; i++) tick(e);
    CHECK_EQ(e.count, 7);
    CHECK(e.playing);
    tick(e);
    CHECK(!e.playing);
    CHECK_EQ(e.count, 7);
}

TEST(colour_and_anim) {
    CHECK_EQ(colorAt(255, 0, 0, 30), 255);
    CHECK_EQ(colorAt(255, 0, 15, 30), 128);
    CHECK_EQ(colorAt(0, 200, 29, 30), 193);
    CHECK_EQ(animCell(2, 4, 3, true, 0), 2);
    CHECK_EQ(animCell(2, 4, 3, true, 11), 5);
    CHECK_EQ(animCell(2, 4, 3, true, 12), 2);
    CHECK_EQ(animCell(2, 4, 3, false, 100), 5);
}

// The golden run SplashEdit's testParticles repeats with its own code: the
// fountain from origin (10, -20, 30), 60 ticks, then a checksum of every live
// particle's position and age.
TEST(golden_run) {
    ParticleEmitterRecord r = fountain();
    r.maxParticles = 64;
    Particle pool[64];
    ParticleEmitterState e = {};
    e.rec = &r;
    e.particles = pool;
    e.origin[0] = 10;
    e.origin[1] = -20;
    e.origin[2] = 30;
    play(e);
    for (int i = 0; i < 60; i++) tick(e);
    uint32_t sum = 0;
    uint32_t slot = e.head;
    for (int k = 0; k < e.count; k++) {
        const Particle& p = pool[slot];
        sum = sum * 31u + (uint32_t)p.pos[0];
        sum = sum * 31u + (uint32_t)p.pos[1];
        sum = sum * 31u + (uint32_t)p.pos[2];
        sum = sum * 31u + p.age;
        slot = slot + 1 >= r.maxParticles ? 0 : slot + 1;
    }
    printf("golden: count %d head %d rng %u sum %u\n", e.count, e.head, e.rng, sum);
    CHECK_EQ(e.count, 15);
    CHECK_EQ(e.head, 18);
    CHECK_EQ(sum, 2719162360u);
}

int main() { return psxsplash::test::runAll(); }
