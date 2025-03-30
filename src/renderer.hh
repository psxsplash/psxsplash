#pragma once

#include <EASTL/array.h>
#include <EASTL/vector.h>

#include <cstdint>
#include <psyqo/bump-allocator.hh>
#include <psyqo/fragments.hh>
#include <psyqo/gpu.hh>
#include <psyqo/kernel.hh>
#include <psyqo/ordering-table.hh>
#include <psyqo/primitives/common.hh>
#include <psyqo/primitives/misc.hh>
#include <psyqo/primitives/triangles.hh>
#include <psyqo/trigonometry.hh>

#include "camera.hh"
#include "gameobject.hh"

namespace psxsplash {

class Renderer final {
  public:
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    static constexpr size_t ORDERING_TABLE_SIZE = 2048 * 3;
    static constexpr size_t BUMP_ALLOCATOR_SIZE = 8096 * 16;

    static void init(psyqo::GPU& gpuInstance);

    void setCamera(Camera& camera);

    void render(eastl::vector<GameObject*>& objects);
    void iterativeSubdivideAndRender(const Tri& initialTri, const eastl::array<psyqo::Vertex, 3>& initialProj,
                                     int zIndex, int maxIterations);
    void vramUpload(const uint16_t* imageData, int16_t posX, int16_t posY, int16_t width, int16_t height);

    static Renderer& getInstance() {
        psyqo::Kernel::assert(instance != nullptr, "Access to renderer was tried without prior initialization");
        return *instance;
    }

  private:
    static Renderer* instance;

    Renderer(psyqo::GPU& gpuInstance) : m_gpu(gpuInstance) {}
    ~Renderer() {}

    Camera* m_currentCamera;

    psyqo::GPU& m_gpu;
    psyqo::Trig<> m_trig;

    psyqo::OrderingTable<ORDERING_TABLE_SIZE> m_ots[2];
    psyqo::Fragments::SimpleFragment<psyqo::Prim::FastFill> m_clear[2];
    psyqo::BumpAllocator<BUMP_ALLOCATOR_SIZE> m_ballocs[2];

    psyqo::Color m_clearcolor = {.r = 63, .g = 63, .b = 100};
};

}  // namespace psxsplash