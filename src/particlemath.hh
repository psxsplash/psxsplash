#pragma once

#include <stdint.h>

// Particle emitters: the pack record and the simulation, free of psyqo and the
// GTE so that tests/host/test_particle.cpp compiles this exact code natively.
// The ParticleSystem (particlesystem.cpp) owns the pool and draws; what a
// particle does from one tick to the next is decided here. SplashEdit's Scene
// view runs the same integer steps (src/editor/particles.cpp in splashedit-ng),
// so its preview and the console agree particle for particle.
//
// Time is counted in ticks of one vsync (1/60 s). Positions are 20.12 world
// units (what GameObject::position holds) shifted left 12 more inside the
// simulation ("P12"), so a slow particle still moves by a fraction of a unit
// each tick; velocities are P12 per tick, gravity P12 per tick per tick, +Y
// down as everywhere on the PS1.
//
// Each tick, in this order:
//   1. every live particle moves by its velocity, then gravity adds to its Y
//      velocity, then it ages by one;
//   2. particles that reached the lifetime are retired. Every particle of an
//      emitter lives the same time and they are born in order, so the pool is
//      a ring: the oldest is always at `head`;
//   3. a playing emitter adds `rate` (16.16 particles per tick) to its
//      accumulator and spawns one particle per whole unit. A one-shot emitter
//      (no loop flag) stops playing after `duration` ticks.
// A spawn takes the emitter's origin and the start velocity plus, per axis, a
// uniform random offset in [-spread, spread] from the emitter's own LCG. A
// spawn into a full pool is dropped.

namespace psxsplash {

/// Emitters a scene can hold; the exporter refuses more.
static constexpr int MAX_PARTICLE_EMITTERS = 32;
/// Particles one emitter can hold; the exporter refuses more.
static constexpr int MAX_PARTICLES_PER_EMITTER = 256;

/// One vsync tick in SceneManager's dt12 units (4096 == one 30 Hz frame).
static constexpr int32_t PARTICLE_TICK_DT = 2048;

enum ParticleFlags : uint8_t {
    PARTICLE_LOOP = 1,      ///< emits until stopped; else for `duration` ticks
    PARTICLE_AUTOPLAY = 2,  ///< plays when the scene loads
};

static constexpr uint16_t PARTICLE_NO_OBJECT = 0xFFFF;
static constexpr uint8_t PARTICLE_NO_ANIM = 0xFF;

/// An emitter as stored in the splashpack (v31). The particle table is a
/// uint16 count and a uint16 pad, then `count` of these, then their names.
struct ParticleEmitterRecord {
    uint32_t nameOffset;     ///< absolute offset of a NUL-terminated name, or 0
    int32_t pos[3];          ///< 20.12 world; relative to the object when following one
    int32_t vel[3];          ///< start velocity, P12 per tick
    int32_t spread[3];       ///< random start velocity, +-P12 per tick per axis
    int32_t gravity;         ///< P12 per tick per tick, +Y (down)
    uint32_t rate;           ///< 16.16 particles per tick
    uint16_t lifetime;       ///< ticks, at least 1
    uint16_t maxParticles;   ///< pool size, 1..MAX_PARTICLES_PER_EMITTER
    uint16_t burst;          ///< spawned at once by Play
    uint16_t duration;       ///< ticks a one-shot emitter emits for
    uint16_t gameObject;     ///< followed game object, or PARTICLE_NO_OBJECT
    uint16_t size;           ///< billboard width, 20.12 world units
    uint8_t sheet;           ///< sprite sheet index
    uint8_t anim;            ///< sprite animation index (global), or PARTICLE_NO_ANIM
    uint8_t cell;            ///< the sheet cell drawn when there is no animation
    uint8_t flags;           ///< ParticleFlags
    uint8_t startColor[3];   ///< tint at birth, 128 neutral
    uint8_t endColor[3];     ///< tint at death
    uint16_t pad;
    uint32_t seed;           ///< the LCG's state at Play
};
static_assert(sizeof(ParticleEmitterRecord) == 76, "ParticleEmitterRecord must be 76 bytes");

/// One live particle. The pool is sized from the pack at load.
struct Particle {
    int32_t pos[3];  ///< P12 world
    int32_t vel[3];  ///< P12 per tick
    uint16_t age;    ///< ticks since birth
    uint16_t pad;
};
static_assert(sizeof(Particle) == 28, "Particle grew; SplashEdit's RAM meter counts 28 bytes each");

/// An emitter's runtime state, beside its pool.
struct ParticleEmitterState {
    const ParticleEmitterRecord* rec;
    Particle* particles;  ///< rec->maxParticles of them, a ring from head
    uint32_t rng;
    uint32_t accum;       ///< 16.16 spawn carry
    uint16_t head;
    uint16_t count;
    uint16_t emitTicks;   ///< ticks played since Play (one-shot stop)
    uint8_t playing;
    uint8_t pad;
    int32_t origin[3];    ///< 20.12 world, this tick's spawn point
};
// 36 bytes with the console's 4-byte pointers (host builds have wider ones).
static_assert(sizeof(void*) != 4 || sizeof(ParticleEmitterState) == 36,
              "ParticleEmitterState grew; SplashEdit's RAM meter counts 36 bytes each");

namespace particlemath {

/// Numerical Recipes' LCG. Returns the next state.
inline uint32_t lcgNext(uint32_t s) { return s * 1664525u + 1013904223u; }

/// A uniform value in [-spread, spread] from the state's high bits.
inline int32_t spreadOf(uint32_t state, int32_t spread) {
    const int32_t r = (int32_t)(state >> 16) - 32768;  // -32768..32767
    return (int32_t)(((int64_t)spread * r) >> 15);
}

/// Bytes of the one block the pool takes for `emitters` emitters holding
/// `particles` particles between them, as the console lays it out (the
/// static_asserts above hold these to the structs on the PS1).
static constexpr uint32_t PARTICLE_STATE_BYTES = 36;
static constexpr uint32_t PARTICLE_BYTES = 28;
inline uint32_t poolBytes(uint32_t emitters, uint32_t particles) {
    return emitters * PARTICLE_STATE_BYTES + particles * PARTICLE_BYTES;
}

inline void spawn(ParticleEmitterState& e) {
    const ParticleEmitterRecord& r = *e.rec;
    if (e.count >= r.maxParticles) return;
    uint32_t slot = (uint32_t)e.head + e.count;
    if (slot >= r.maxParticles) slot -= r.maxParticles;
    Particle& p = e.particles[slot];
    for (int a = 0; a < 3; a++) {
        p.pos[a] = e.origin[a] * 4096;
        e.rng = lcgNext(e.rng);
        p.vel[a] = r.vel[a] + spreadOf(e.rng, r.spread[a]);
    }
    p.age = 0;
    p.pad = 0;
    e.count++;
}

/// Play: emission restarts, the LCG goes back to the seed, the burst is spawned.
inline void play(ParticleEmitterState& e) {
    e.playing = 1;
    e.emitTicks = 0;
    e.accum = 0;
    e.rng = e.rec->seed;
    for (uint16_t i = 0; i < e.rec->burst; i++) spawn(e);
}

/// Stop: no more spawns; the live particles finish their lives.
inline void stop(ParticleEmitterState& e) { e.playing = 0; }

/// Kill every live particle at once.
inline void clear(ParticleEmitterState& e) {
    e.head = 0;
    e.count = 0;
}

/// Spawns n now; stops once the pool is full (a huge n from Lua costs nothing).
inline void burst(ParticleEmitterState& e, uint32_t n) {
    for (uint32_t i = 0; i < n && e.count < e.rec->maxParticles; i++) spawn(e);
}

/// One vsync tick (see the top of this file).
inline void tick(ParticleEmitterState& e) {
    const ParticleEmitterRecord& r = *e.rec;
    const uint16_t max = r.maxParticles;
    uint32_t slot = e.head;
    for (uint16_t i = 0; i < e.count; i++) {
        Particle& p = e.particles[slot];
        p.pos[0] += p.vel[0];
        p.pos[1] += p.vel[1];
        p.pos[2] += p.vel[2];
        p.vel[1] += r.gravity;
        p.age++;
        if (++slot >= max) slot = 0;
    }
    while (e.count > 0 && e.particles[e.head].age >= r.lifetime) {
        if (++e.head >= max) e.head = 0;
        e.count--;
    }
    if (!e.playing) return;
    if (!(r.flags & PARTICLE_LOOP) && e.emitTicks >= r.duration) {
        e.playing = 0;
        return;
    }
    e.emitTicks++;
    e.accum += r.rate;
    while (e.accum >= 65536) {
        e.accum -= 65536;
        spawn(e);
    }
}

/// Tint at `age`: start to end, linear over the lifetime.
inline uint8_t colorAt(uint8_t start, uint8_t end, uint16_t age, uint16_t lifetime) {
    if (lifetime == 0) return start;
    return (uint8_t)((int32_t)start + ((int32_t)end - (int32_t)start) * (int32_t)age / (int32_t)lifetime);
}

/// Which cell of an animation a particle shows at `age` (ticks are vsyncs,
/// the animation's frameDuration is in vsyncs too).
inline uint8_t animCell(uint8_t firstFrame, uint8_t frameCount, uint8_t frameDuration, bool loop, uint16_t age) {
    if (frameCount == 0) return firstFrame;
    uint32_t i = frameDuration ? age / frameDuration : 0;
    if (loop) i %= frameCount;
    else if (i >= frameCount) i = frameCount - 1u;
    return (uint8_t)(firstFrame + i);
}

/// Copies at most `max` records of a particle table (`table` points at its
/// count) and returns how many; `out` receives pointers into the table.
inline int parseTable(const uint8_t* table, const ParticleEmitterRecord** out, int max) {
    const uint16_t count = (uint16_t)(table[0] | (table[1] << 8));
    int n = count < max ? count : max;
    const ParticleEmitterRecord* recs = reinterpret_cast<const ParticleEmitterRecord*>(table + 4);
    for (int i = 0; i < n; i++) out[i] = &recs[i];
    return n;
}

}  // namespace particlemath
}  // namespace psxsplash
