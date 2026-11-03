#include "particlesystem.hh"

#include <psyqo/gte-kernels.hh>
#include <psyqo/gte-registers.hh>
#include <psyqo/kernel.hh>
#include <psyqo/primitives/quads.hh>
#include <psyqo/xprintf.h>

#include "gameobject.hh"
#include "spritemath.hh"
#include "spritesystem.hh"
#include "streq.hh"

using namespace psyqo::GTE;

namespace psxsplash {

static_assert(sizeof(psyqo::Fragments::SimpleFragment<psyqo::Prim::TexturedQuad>) == PARTICLE_BUMP_BYTES,
              "a particle's quad fragment changed size; SplashEdit's bump estimate counts PARTICLE_BUMP_BYTES");
static_assert(sizeof(ParticleEmitterState) == particlemath::PARTICLE_STATE_BYTES && sizeof(Particle) == particlemath::PARTICLE_BYTES,
              "particlemath::poolBytes no longer matches the structs");

void ParticleSystem::load(const uint8_t* data, const uint8_t* table, SpriteSystem* sprites,
                          eastl::vector<GameObject*>& objects) {
    unload();
    m_data = data;
    m_sprites = sprites;
    m_objects = &objects;
    const ParticleEmitterRecord* recs[MAX_PARTICLE_EMITTERS];
    const int n = particlemath::parseTable(table, recs, MAX_PARTICLE_EMITTERS);
    if (n <= 0) return;
    uint32_t particles = 0;
    for (int i = 0; i < n; i++) {
        psyqo::Kernel::assert(recs[i]->maxParticles >= 1 && recs[i]->maxParticles <= MAX_PARTICLES_PER_EMITTER,
                              "Splashpack particle emitter pool out of range");
        particles += recs[i]->maxParticles;
    }
    m_poolBytes = particlemath::poolBytes(n, particles);
    m_pool = new uint8_t[m_poolBytes];
    m_emitters = reinterpret_cast<ParticleEmitterState*>(m_pool);
    Particle* next = reinterpret_cast<Particle*>(m_pool + n * sizeof(ParticleEmitterState));
    for (int i = 0; i < n; i++) {
        ParticleEmitterState& e = m_emitters[i];
        e = {};
        e.rec = recs[i];
        e.particles = next;
        next += recs[i]->maxParticles;
    }
    m_count = n;
    m_tickAccum = 0;
    for (int i = 0; i < n; i++) {
        refreshOrigin(m_emitters[i]);
        if (m_emitters[i].rec->flags & PARTICLE_AUTOPLAY) particlemath::play(m_emitters[i]);
    }
    printf("Particles: %d emitters, %u particles, pool %u bytes\n", n, (unsigned)particles, (unsigned)m_poolBytes);
}

void ParticleSystem::unload() {
    delete[] m_pool;
    m_pool = nullptr;
    m_poolBytes = 0;
    m_emitters = nullptr;
    m_count = 0;
    m_data = nullptr;
    m_objects = nullptr;
}

void ParticleSystem::refreshOrigin(ParticleEmitterState& e) {
    const ParticleEmitterRecord& r = *e.rec;
    int32_t base[3] = {0, 0, 0};
    if (r.gameObject != PARTICLE_NO_OBJECT && m_objects && r.gameObject < m_objects->size()) {
        const GameObject* o = (*m_objects)[r.gameObject];
        if (o) {
            base[0] = o->position.x.value;
            base[1] = o->position.y.value;
            base[2] = o->position.z.value;
        }
    }
    for (int a = 0; a < 3; a++) e.origin[a] = base[a] + r.pos[a];
}

void ParticleSystem::update(int32_t dt12) {
    if (m_count == 0) return;
    m_tickAccum += dt12;
    // SceneManager clamps dt12 to four 30 Hz frames, so at most 8 ticks.
    while (m_tickAccum >= PARTICLE_TICK_DT) {
        m_tickAccum -= PARTICLE_TICK_DT;
        for (int i = 0; i < m_count; i++) {
            ParticleEmitterState& e = m_emitters[i];
            refreshOrigin(e);
            particlemath::tick(e);
        }
    }
}

int ParticleSystem::find(const char* name) const {
    if (!name || !m_data) return -1;
    for (int i = 0; i < m_count; i++) {
        const uint32_t off = m_emitters[i].rec->nameOffset;
        if (off && streq(reinterpret_cast<const char*>(m_data + off), name)) return i;
    }
    return -1;
}

void ParticleSystem::play(int i) {
    if (!valid(i)) return;
    refreshOrigin(m_emitters[i]);
    particlemath::play(m_emitters[i]);
}

void ParticleSystem::stop(int i) {
    if (valid(i)) particlemath::stop(m_emitters[i]);
}

void ParticleSystem::clear(int i) {
    if (valid(i)) particlemath::clear(m_emitters[i]);
}

void ParticleSystem::burst(int i, uint32_t n) {
    if (!valid(i)) return;
    refreshOrigin(m_emitters[i]);
    particlemath::burst(m_emitters[i], n);
}

bool ParticleSystem::isPlaying(int i) const { return valid(i) && m_emitters[i].playing; }

int ParticleSystem::liveParticles(int i) const { return valid(i) ? m_emitters[i].count : 0; }

void ParticleSystem::renderOT(const psyqo::Matrix33& rotation, const psyqo::Vec3& cameraPosition, int32_t projH, int32_t nearSZ, int32_t otSize,
                              int32_t fogFarSZ, Renderer::OT& ot, Renderer::Balloc& balloc) {
    if (m_count == 0 || !m_sprites) return;
    // The camera alone: a world position goes straight into V0.
    writeSafe<PseudoRegister::Rotation>(rotation);
    writeSafe<PseudoRegister::Translation>(cameraPosition);

    for (int ei = 0; ei < m_count; ei++) {
        const ParticleEmitterState& e = m_emitters[ei];
        if (e.count == 0) continue;
        const ParticleEmitterRecord& r = *e.rec;
        const SpriteAnim* anim = r.anim != PARTICLE_NO_ANIM ? m_sprites->anim(r.anim) : nullptr;
        const SpriteSheet* sheet = m_sprites->sheet(anim ? anim->sheet : r.sheet);
        if (!sheet || sheet->cellW == 0 || sheet->cellH == 0) continue;

        psyqo::PrimPieces::TPageAttr tpage;
        tpage.setPageX(sheet->texpageX);
        tpage.setPageY(sheet->texpageY);
        switch (sheet->bitDepth) {
            case 0: tpage.set(psyqo::Prim::TPageAttr::Tex4Bits); break;
            case 1: tpage.set(psyqo::Prim::TPageAttr::Tex8Bits); break;
            default: tpage.set(psyqo::Prim::TPageAttr::Tex16Bits); break;
        }
        // Dithering stays on: untextured triangles drawn after this read it
        // from the GPU's current texpage.
        tpage.setDithering(true);
        tpage.set(psyqo::Prim::TPageAttr::FullBackAndFullFront);
        // clutX is already in 16-pixel units, as for sprites.
        const psyqo::PrimPieces::ClutIndex clut(sheet->clutX, sheet->clutY);
        // Height follows the cell's shape.
        const int32_t sizeW = r.size, sizeH = (int32_t)r.size * sheet->cellH / sheet->cellW;

        uint32_t slot = e.head;
        for (uint16_t k = 0; k < e.count; k++, slot = slot + 1 >= r.maxParticles ? 0 : slot + 1) {
            const Particle& p = e.particles[slot];
            const int32_t wx = p.pos[0] >> 12, wy = p.pos[1] >> 12, wz = p.pos[2] >> 12;
            if (wx < -32768 || wx > 32767 || wy < -32768 || wy > 32767 || wz < -32768 || wz > 32767) continue;
            psyqo::GTE::PackedVec3 v;
            v.x.value = (int16_t)wx;
            v.y.value = (int16_t)wy;
            v.z.value = (int16_t)wz;
            writeSafe<PseudoRegister::V0>(v);
            Kernels::rtps();
            uint32_t uz;
            read<Register::SZ3>(&uz);
            const int32_t sz = (int32_t)uz;
            // Too near for an exact projection, past the table or the fog wall.
            if (sz <= nearSZ || sz >= otSize) continue;
            if (fogFarSZ > 0 && sz >= fogFarSZ) continue;
            psyqo::Vertex c;
            read<Register::SXY2>(&c.packed);
            int32_t w = sizeW * projH / sz, h = sizeH * projH / sz;
            if (w < 1) w = 1;
            if (h < 1) h = 1;
            if (w > 256) w = 256;
            if (h > 256) h = 256;
            const int32_t x0 = c.x - w / 2, y0 = c.y - h / 2;
            if (x0 + w < 0 || x0 >= 320 || y0 + h < 0 || y0 >= 240) continue;
            if (balloc.remaining() < PARTICLE_BUMP_BYTES * 4) return;

            uint8_t u, vv;
            const uint8_t frame = anim ? particlemath::animCell(anim->firstFrame, anim->frameCount, anim->frameDuration,
                                                                 anim->loop, p.age)
                                       : r.cell;
            spritemath::frameToUV(*sheet, frame, u, vv);
            const uint8_t uR = (uint8_t)(u + sheet->cellW - 1), vB = (uint8_t)(vv + sheet->cellH - 1);
            const psyqo::Color col = {.r = particlemath::colorAt(r.startColor[0], r.endColor[0], p.age, r.lifetime),
                                      .g = particlemath::colorAt(r.startColor[1], r.endColor[1], p.age, r.lifetime),
                                      .b = particlemath::colorAt(r.startColor[2], r.endColor[2], p.age, r.lifetime)};

            auto& q = balloc.allocateFragment<psyqo::Prim::TexturedQuad>();
            q.primitive.setColor(col);
            q.primitive.pointA = {{.x = (int16_t)x0, .y = (int16_t)y0}};
            q.primitive.pointB = {{.x = (int16_t)(x0 + w), .y = (int16_t)y0}};
            q.primitive.pointC = {{.x = (int16_t)x0, .y = (int16_t)(y0 + h)}};
            q.primitive.pointD = {{.x = (int16_t)(x0 + w), .y = (int16_t)(y0 + h)}};
            q.primitive.uvA.u = u;
            q.primitive.uvA.v = vv;
            q.primitive.uvB.u = uR;
            q.primitive.uvB.v = vv;
            q.primitive.uvC.u = u;
            q.primitive.uvC.v = vB;
            q.primitive.uvD.u = uR;
            q.primitive.uvD.v = vB;
            q.primitive.clutIndex = clut;
            q.primitive.tpage = tpage;
            int32_t z = sz;
            if (z < Renderer::WORLD_DEPTH_MIN) z = Renderer::WORLD_DEPTH_MIN;
            ot.insert(q, z);
        }
    }
}

}  // namespace psxsplash
