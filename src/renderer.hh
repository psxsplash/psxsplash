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
#include "lightmath.hh"
#include "skinmesh.hh"
#include "triclip.hh"

namespace psxsplash {

class UISystem; // Forward declaration
class SpriteSystem; // Forward declaration
class TileSystem;   // Forward declaration
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

    // Ordering-table depth bands.
    //
    // Depth 0 is drawn LAST, i.e. frontmost; larger depths sit further back.
    // Within a single depth the OT inserts at the head, so the last primitive
    // inserted is the first one drawn.
    //
    // The 2D overlays get slots that 3D geometry can never occupy. Without that,
    // a polygon right in front of the camera lands on the same depth as a sprite
    // and which one survives depends on insertion order - the UI only gets away
    // with sharing depth 0/1 today because it happens to be inserted last.
    //
    //   0 .. UI_DEPTH_MAX                  UI
    //   SPRITE_DEPTH_BASE .. +LAYERS-1     screen-space sprites, layer 0 in front
    //   WORLD_DEPTH_MIN .. OT_SIZE-1       3D geometry
    //
    // Cost: WORLD_DEPTH_MIN (14) OT slots, and near-camera polygons clamp to
    // WORLD_DEPTH_MIN instead of 1 - which is what stops them fighting the HUD.
    static constexpr int UI_DEPTH_MAX = 1;
    static constexpr int SPRITE_DEPTH_BASE = UI_DEPTH_MAX + 1;
    /// How many sprite layers content may use. SpriteSystem::setLayer clamps a
    /// higher layer to SPRITE_LAYERS - 1, so content that assigns more layers
    /// than this gets them merged into the backmost one. Keep this above the
    /// highest layer a game uses.
    ///
    /// Cost is one ordering-table slot per layer, so headroom is cheap.
    static constexpr int SPRITE_LAYERS = 12;
    static constexpr int WORLD_DEPTH_MIN = SPRITE_DEPTH_BASE + SPRITE_LAYERS;

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
    void SetSpriteSystem(SpriteSystem* sprites) { m_spriteSystem = sprites; }
    void SetTileSystem(TileSystem* tiles) { m_tileSystem = tiles; }
#ifdef PSXSPLASH_MEMOVERLAY
    void SetMemOverlay(MemOverlay* overlay) { m_memOverlay = overlay; }
#endif
    psyqo::GPU& getGPU() { return m_gpu; }

    void SetSkinData(const SkinAnimSet* sets, const SkinAnimState* states, int count) {
        m_skinSets = sets; m_skinStates = states; m_skinCount = count;
    }

    /// The scene's point lights. Read every frame, so Lua edits take effect on
    /// the next one. nullptr / 0 turns dynamic lighting off.
    void SetPointLights(const PointLight* lights, int count) {
        m_lights = lights; m_lightCount = count;
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
    SpriteSystem* m_spriteSystem = nullptr;
    TileSystem* m_tileSystem = nullptr;
#ifdef PSXSPLASH_MEMOVERLAY
    MemOverlay* m_memOverlay = nullptr;
#endif

    const SkinAnimSet* m_skinSets = nullptr;
    const SkinAnimState* m_skinStates = nullptr;
    int m_skinCount = 0;

    const PointLight* m_lights = nullptr;
    int m_lightCount = 0;
    // Lights reaching the object setupObjectTransform last loaded. Count 0
    // leaves processTriangle on the unlit path.
    lightmath::ObjectLights m_objLights;
    bool m_objLightsSmooth = false;

    // Per scene light, its flat-path colour for every quantised t
    // (lightmath::flatColour), rebuilt only when Lua changes its colour.
    struct ColourLut {
        uint32_t key = 0xFFFFFFFF;  ///< r | g << 8 | b << 16, plus intensity below
        uint16_t intensity = 0;
        uint32_t rgb[lightmath::kColourLutSize];
    };
    ColourLut m_colourLuts[MAX_SCENE_LIGHTS];
    const uint32_t* colourLutFor(int sceneIndex);

    TriangleRef m_visibleRefs[MAX_VISIBLE_TRIANGLES];
    int m_frameCount = 0;

    psyqo::Vec3 computeCameraViewPos();
    void setupObjectTransform(GameObject* obj, const psyqo::Vec3& cameraPosition);
    void prepareObjectLights(const GameObject* obj);

    void processTriangle(Tri& tri, int32_t fogFarSZ,
                         psyqo::OrderingTable<ORDERING_TABLE_SIZE>& ot,
                         psyqo::BumpAllocator<BUMP_ALLOCATOR_SIZE>& balloc,
                         int depth = 0,
                         psyqo::PrimPieces::UVCoords uvOffset = { 0 });

    void renderSkinnedObjects(eastl::vector<GameObject*>& objects,
                              const psyqo::Vec3& cameraPosition,
                              int32_t fogFarSZ,
                              psyqo::OrderingTable<ORDERING_TABLE_SIZE>& ot,
                              psyqo::BumpAllocator<BUMP_ALLOCATOR_SIZE>& balloc,
                              const Frustum* frustum = nullptr);
};

}  // namespace psxsplash
