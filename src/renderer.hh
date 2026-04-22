#pragma once

#include <EASTL/array.h>
#include <EASTL/vector.h>

#include <psyqo/bump-allocator.hh>
#include <psyqo/fragments.hh>
#include <psyqo/gpu.hh>
#include <psyqo/kernel.hh>
#include <psyqo/ordering-table.hh>
#include <psyqo/primitives/common.hh>
#include <psyqo/primitives/misc.hh>
#include <psyqo/primitives/triangles.hh>
#include <psyqo/trigonometry.hh>

#include "bvh.hh"
#include "camera.hh"
#include "gameobject.hh"
#include "skinmesh.hh"
#include "triclip.hh"

namespace psxsplash {

class UISystem; // Forward declaration
#ifdef PSXSPLASH_MEMOVERLAY
class MemOverlay; // Forward declaration
#endif

struct FogConfig {
    bool enabled = false;
    psyqo::Color color = {.r = 0, .g = 0, .b = 0};
    uint8_t density = 5;
    int32_t fogFarSZ = 0;
};

class Renderer final {
  public:
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

#ifndef OT_SIZE
#define OT_SIZE (2048 * 8)
#endif
#ifndef BUMP_SIZE
#define BUMP_SIZE (8096 * 24)
#endif
    static constexpr size_t ORDERING_TABLE_SIZE = OT_SIZE;
    static constexpr size_t BUMP_ALLOCATOR_SIZE = BUMP_SIZE;
    static constexpr size_t MAX_VISIBLE_TRIANGLES = 4096;

    static constexpr int32_t PROJ_H = 120;
    static constexpr int32_t SCREEN_CX = 160;
    static constexpr int32_t SCREEN_CY = 120;

    static void Init(psyqo::GPU& gpuInstance);
    void SetCamera(Camera& camera);
    void SetFog(const FogConfig& fog);

    void Render(eastl::vector<GameObject*>& objects);
    void RenderWithBVH(eastl::vector<GameObject*>& objects, const BVHManager& bvh);
    void RenderWithRooms(eastl::vector<GameObject*>& objects,
                         const RoomData* rooms, int roomCount,
                         const PortalData* portals, int portalCount,
                         const TriangleRef* roomTriRefs,
                         const RoomCell* cells = nullptr,
                         const RoomPortalRef* roomPortalRefs = nullptr,
                         int cameraRoom = -1);

    void VramUpload(const uint16_t* imageData, int16_t posX, int16_t posY,
                    int16_t width, int16_t height);

    void SetUISystem(UISystem* ui) { m_uiSystem = ui; }
#ifdef PSXSPLASH_MEMOVERLAY
    void SetMemOverlay(MemOverlay* overlay) { m_memOverlay = overlay; }
#endif
    psyqo::GPU& getGPU() { return m_gpu; }

    void SetSkinData(const SkinAnimSet* sets, const SkinAnimState* states, int count) {
        m_skinSets = sets; m_skinStates = states; m_skinCount = count;
    }

    static Renderer& GetInstance() {
        psyqo::Kernel::assert(instance != nullptr,
                              "Access to renderer was tried without prior initialization");
        return *instance;
    }

  private:
    static Renderer* instance;

    Renderer(psyqo::GPU& gpuInstance) : m_gpu(gpuInstance) {}
    ~Renderer() {}

    Camera* m_currentCamera = nullptr;
    psyqo::GPU& m_gpu;
    psyqo::Trig<> m_trig;

    psyqo::OrderingTable<ORDERING_TABLE_SIZE> m_ots[2];
    psyqo::Fragments::SimpleFragment<psyqo::Prim::FastFill> m_clear[2];
    psyqo::BumpAllocator<BUMP_ALLOCATOR_SIZE> m_ballocs[2];

    FogConfig m_fog;
    psyqo::Color m_clearcolor = {.r = 0, .g = 0, .b = 0};

    UISystem* m_uiSystem = nullptr;
#ifdef PSXSPLASH_MEMOVERLAY
    MemOverlay* m_memOverlay = nullptr;
#endif

    const SkinAnimSet* m_skinSets = nullptr;
    const SkinAnimState* m_skinStates = nullptr;
    int m_skinCount = 0;

    TriangleRef m_visibleRefs[MAX_VISIBLE_TRIANGLES];
    int m_frameCount = 0;

    psyqo::Vec3 computeCameraViewPos();
    void setupObjectTransform(GameObject* obj, const psyqo::Vec3& cameraPosition);

    void processTriangle(Tri& tri, int32_t fogFarSZ,
                         psyqo::OrderingTable<ORDERING_TABLE_SIZE>& ot,
                         psyqo::BumpAllocator<BUMP_ALLOCATOR_SIZE>& balloc,
                         int depth = 0,
                         psyqo::PrimPieces::UVCoords uvOffset = {});

    void renderSkinnedObjects(eastl::vector<GameObject*>& objects,
                              const psyqo::Vec3& cameraPosition,
                              int32_t fogFarSZ,
                              psyqo::OrderingTable<ORDERING_TABLE_SIZE>& ot,
                              psyqo::BumpAllocator<BUMP_ALLOCATOR_SIZE>& balloc,
                              const Frustum* frustum = nullptr);
};

}  // namespace psxsplash
