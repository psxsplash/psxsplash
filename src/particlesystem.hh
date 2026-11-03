#pragma once

#include <stdint.h>

#include <EASTL/vector.h>

#include "particlemath.hh"
#include "renderer.hh"

namespace psxsplash {

class GameObject;
class SpriteSystem;

/// Billboard particle emitters (splashpack v31): camera-facing quads cut from
/// the scene's sprite sheets, drawn in the ordering table at their own depth
/// among the 3D geometry. The simulation is particlemath.hh's.
///
/// Memory: one block allocated when the scene loads, sized from the pack
/// (particlemath::poolBytes: every emitter's state plus its maxParticles), and
/// freed with the scene. Nothing is allocated per frame. Each drawn particle
/// costs one TexturedQuad fragment (PARTICLE_BUMP_BYTES) of the frame's bump
/// allocator, which the exporter adds to the size it writes.
class ParticleSystem {
  public:
    /// Parses the table and allocates the pool; emitters with the autoplay
    /// flag start. `table` points at the particle table's count.
    void load(const uint8_t* data, const uint8_t* table, SpriteSystem* sprites,
              eastl::vector<GameObject*>& objects);
    /// Frees the pool. Safe to call when nothing is loaded.
    void unload();

    /// Advances every emitter by whole vsync ticks of dt12 (the carry is kept).
    void update(int32_t dt12);

    /// Emitter by name (its object's name), or -1.
    int find(const char* name) const;
    int count() const { return m_count; }
    void play(int i);
    void stop(int i);
    void clear(int i);
    void burst(int i, uint32_t n);
    bool isPlaying(int i) const;
    int liveParticles(int i) const;

    /// Inserts this frame's quads. Called by the renderer after the 3D
    /// geometry: `rotation` is the camera's, `cameraPosition` its view-space
    /// translation (Renderer::computeCameraViewPos).
    void renderOT(const psyqo::Matrix33& rotation, const psyqo::Vec3& cameraPosition, int32_t projH, int32_t nearSZ, int32_t otSize,
                  int32_t fogFarSZ, Renderer::OT& ot, Renderer::Balloc& balloc);

    /// Bytes the pool took at load (0 when nothing is loaded).
    uint32_t poolBytes() const { return m_poolBytes; }

  private:
    bool valid(int i) const { return i >= 0 && i < m_count; }
    void refreshOrigin(ParticleEmitterState& e);

    const uint8_t* m_data = nullptr;
    SpriteSystem* m_sprites = nullptr;
    eastl::vector<GameObject*>* m_objects = nullptr;
    uint8_t* m_pool = nullptr;
    uint32_t m_poolBytes = 0;
    ParticleEmitterState* m_emitters = nullptr;
    int m_count = 0;
    int32_t m_tickAccum = 0;
};

/// TexturedQuad fragment bytes, what one drawn particle takes of the frame's
/// bump allocator (static_assert in particlesystem.cpp).
static constexpr uint32_t PARTICLE_BUMP_BYTES = 40;

}  // namespace psxsplash
