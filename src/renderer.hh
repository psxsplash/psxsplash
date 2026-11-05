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
#include "features.hh"
#include "lightmath.hh"
#include "shadowmath.hh"
#include "skinmesh.hh"
#include "triclip.hh"

namespace psxsplash {

class UISystem; // Forward declaration
class SpriteSystem; // Forward declaration
class TileSystem;   // Forward declaration
class NavRegionSystem;
class BackgroundSystem;
#ifdef PSXSPLASH_MEMOVERLAY
class MemOverlay; // Forward declaration
#endif

struct FogConfig {
    bool enabled = false;
    psyqo::Color color = {.r = 0, .g = 0, .b = 0};
    uint8_t density = 5;
    int32_t fogFarSZ = 0;
    // Fade everything to the background colour just before the edge where it
    // stops being drawn (fade.hh edgeDepth), fog on or off: the pack's
    // render flags bit 0.
    bool edgeFade = false;
    // Fog mask CLUTs (CLUT attribute words, 0 = none) for 4bpp and 8bpp
    // textures: entry 0 transparent, every other entry white with STP set.
    uint16_t maskClut4 = 0;
    uint16_t maskClut8 = 0;
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
    // Used when a splashpack does not carry its own sizes (v24 and older).
    static constexpr uint32_t DEFAULT_ORDERING_TABLE_SIZE = OT_SIZE;
    static constexpr uint32_t DEFAULT_BUMP_ALLOCATOR_SIZE = BUMP_SIZE;

    using OT = psyqo::DynamicOrderingTable<>;
    using Balloc = psyqo::DynamicBumpAllocator<>;

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
    /// Allocates the ordering tables and bump allocators. Only the first call
    /// allocates: a scene load happens while the GPU still owes the previous
    /// frame's chain and an ordering-table clear on these buffers, so they are
    /// never freed. Every scene of a game carries the same sizes, the largest
    /// any of them needs; a later scene asking for more is an export mistake.
    void Configure(uint32_t otSize, uint32_t bumpSize);
    /// Sizes the frustum culling output for a scene's BVH, which never yields
    /// more triangles than it references. The buffer only grows.
    void ReserveVisibleTriangles(uint32_t count);
    uint32_t OrderingTableSize() const { return m_otSize; }
    uint32_t BumpAllocatorSize() const { return m_bumpSize; }
    /// Most bump allocator bytes any frame has used so far.
    uint32_t PeakBumpUse() const { return m_peakBump; }
    /// Largest ordering-table depth any triangle has asked for so far, drawn or not.
    int32_t PeakDepth() const { return m_peakZ; }
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
    void SetBackgroundSystem(BackgroundSystem* bg) { m_backgroundSystem = bg; }
#ifdef PSXSPLASH_MEMOVERLAY
    void SetMemOverlay(MemOverlay* overlay) { m_memOverlay = overlay; }
#endif
    psyqo::GPU& getGPU() { return m_gpu; }

#if PSXSPLASH_FEATURE_SKIN
    void SetSkinData(const SkinAnimSet* sets, const SkinAnimState* states, int count) {
        m_skinSets = sets; m_skinStates = states; m_skinCount = count;
    }
#endif

#if PSXSPLASH_FEATURE_LIGHTS
    /// The scene's point lights. Read every frame, so Lua edits take effect on
    /// the next one. nullptr / 0 turns dynamic lighting off.
    void SetPointLights(const PointLight* lights, int count) {
        m_lights = lights; m_lightCount = count;
    }
#endif

#if PSXSPLASH_FEATURE_SHADOWS
    /// The scene's blob shadows (shadowmath.hh). nullptr / 0 draws none.
    void SetShadows(const ShadowRecord* shadows, int count) {
        m_shadows = shadows; m_shadowCount = count;
    }
    /// The floor shadows land on. nullptr (or nav not loaded) puts each blob
    /// flat under its object's bounds.
    void SetShadowFloor(const NavRegionSystem* nav) { m_shadowNav = nav; }
#endif

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

    OT* m_ots[2] = {nullptr, nullptr};
    psyqo::Fragments::SimpleFragment<psyqo::Prim::FastFill> m_clear[2];
    Balloc* m_ballocs[2] = {nullptr, nullptr};
    uint32_t m_otSize = 0;
    uint32_t m_bumpSize = 0;
    uint32_t m_peakBump = 0;
    int32_t m_peakZ = 0;
    int32_t m_reportedZ = 0;
    uint32_t m_reportedBump = 0;
#ifdef PCDRV_SUPPORT
    // Triangles the renderer lost or bent since the last report, by cause:
    // dropped past the ordering table, `skinNear` skinned ones whose view-space
    // edges do not fit the 16 bits the clipper takes them in (they went
    // through the static path's clipper otherwise, and so count in `clip` and
    // `split` too), `leaf` (no longer counted: kept for the report's format),
    // and `clamp` (no longer counted either: nothing clamps). Out of `tris` considered;
    // `clip` counts the ones that went through clipTriangle (not a loss),
    // `split` the ones split against affine warp (not a loss either), `eqLo`
    // and `eqHi` the lowest and highest split threshold the frames used, in
    // quarter pixels, and `refused` the splits refused to keep the bump
    // allocator from running out (drawn warped; may leave a T-junction).
    struct LostCounts {
        uint32_t ot = 0, skinNear = 0, leaf = 0, clamp = 0, tris = 0, clip = 0, split = 0, refused = 0;
        uint32_t eqLo = 0xffffffff, eqHi = 0;
    };
    LostCounts m_lost;
#endif
    void reportPeaks();
    void notePeakBump(const Balloc& balloc) {
        if (balloc.used() > m_peakBump) m_peakBump = balloc.used();
    }

    FogConfig m_fog;
    // This frame's fade edge in SZ units (fade.hh edgeDepth), 0 = edge fade off.
    int32_t m_edgeSZ = 0;
    // This frame's depth below which nothing fades (fade.hh fadeStart).
    int32_t m_fadeStartSZ = 0x7fffffff;
    void beginFade(int32_t fogFarSZ);
    static void fadeTriangle(int32_t sz0, int32_t sz1, int32_t sz2, int32_t fogFarSZ, int32_t edge, int32_t fogIR[3]);
    psyqo::Color m_clearcolor = {.r = 0, .g = 0, .b = 0};

    UISystem* m_uiSystem = nullptr;
    SpriteSystem* m_spriteSystem = nullptr;
    TileSystem* m_tileSystem = nullptr;
    BackgroundSystem* m_backgroundSystem = nullptr;
#ifdef PSXSPLASH_MEMOVERLAY
    MemOverlay* m_memOverlay = nullptr;
#endif

#if PSXSPLASH_FEATURE_SKIN
    const SkinAnimSet* m_skinSets = nullptr;
    const SkinAnimState* m_skinStates = nullptr;
    int m_skinCount = 0;
#endif

#if PSXSPLASH_FEATURE_LIGHTS
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
    void prepareObjectLights(const GameObject* obj);
#endif

#if PSXSPLASH_FEATURE_SHADOWS
    const ShadowRecord* m_shadows = nullptr;
    int m_shadowCount = 0;
    const NavRegionSystem* m_shadowNav = nullptr;
    void renderShadows(eastl::vector<GameObject*>& objects, const psyqo::Vec3& cameraPosition,
                       int32_t fogFarSZ, OT& ot, Balloc& balloc);
#endif

    TriangleRef* m_visibleRefs = nullptr;
    uint32_t m_visibleCapacity = 0;
    int m_frameCount = 0;

    psyqo::Vec3 computeCameraViewPos();
    void setupObjectTransform(GameObject* obj, const psyqo::Vec3& cameraPosition);

    // The additive fog pass over a textured triangle.
    void insertFogPass(const Tri& tri, const psyqo::Vertex& a, const psyqo::Vertex& b, const psyqo::Vertex& c,
                       psyqo::PrimPieces::UVCoords uvOffset, const int32_t fogIR[3],
                       OT& ot, Balloc& balloc, int32_t zIndex);

    void processTriangle(Tri& tri, int32_t fogFarSZ,
                         OT& ot,
                         Balloc& balloc,
                         psyqo::PrimPieces::UVCoords uvOffset = { 0 }, int depth = 0);
    // Splits tri at the midpoints of the edges in `edges` (bit i: corner i to
    // corner i + 1) into 2, 3 or 4 children and draws them through
    // processTriangle at depth + 1; or returns false, leaving tri to the
    // caller, when the bump allocator is nearly full.
    bool splitTriangle(const Tri& tri, unsigned edges, int32_t fogFarSZ, OT& ot, Balloc& balloc,
                       psyqo::PrimPieces::UVCoords uvOffset, int depth);
    // Fog, lights and the primitives of one projected triangle (both paths;
    // inlined into both, so the per-triangle path pays no call for it).
    __attribute__((always_inline)) inline void emitTriangle(const Tri& tri, const psyqo::Vertex& p0, const psyqo::Vertex& p1, const psyqo::Vertex& p2,
                      const int32_t sz[3], int32_t zIndex, int32_t fogFarSZ, OT& ot, Balloc& balloc,
                      psyqo::PrimPieces::UVCoords uvOffset);
    // Clips tri, whose vertices are loaded in V0-V2 and projected by the GTE
    // into `projected`, to the near plane and the rasterizer's safe box, and
    // draws the pieces, projected in software where the GTE could not.
    void clipTriangle(const Tri& tri, const psyqo::Vertex projected[3], int32_t fogFarSZ, OT& ot,
                      Balloc& balloc, psyqo::PrimPieces::UVCoords uvOffset);
    // The GTE projects a vertex exactly only from SZ = (H >> 1) + 1: at
    // SZ <= H/2 its H/SZ division overflows and SXY comes out as if H/SZ were
    // 2. Triangles with a vertex nearer than that are clipped and projected
    // in software. No margin is needed: SZ is the GTE's own integer.
    int32_t m_nearSZ = (PROJ_H >> 1) + 1;
    // The clipper's near plane, in SZ units; and the inset of its box from the
    // rasterizer's safe box, in pixels, for the rounding of the projection.
    static constexpr int32_t CLIP_NEAR_Z = 8;
    static constexpr int32_t CLIP_BOX_MARGIN = 4;
    void setProjectionH(int32_t h);
    // Affine-warp split. The GPU interpolates texture coordinates linearly on
    // screen; along an edge of screen length L between view depths z0 and z1
    // the texel the 3D midpoint carries lands L*|z1 - z0| / (2*(z0 + z1))
    // pixels from the screen midpoint where it belongs. An edge further off
    // than m_splitQ quarter pixels is split at its model-space midpoint,
    // decided from its two endpoints alone, so both triangles sharing it agree;
    // down to SPLIT_DEPTH levels. Measured on the courtyard, lamplighter and
    // the walk worlds: 8 pixels (SPLIT_ERR_Q) and 3 levels clear the near
    // chevrons; 4 pixels or a fourth level cost frames without a visible
    // difference near the camera.
    static constexpr int32_t SPLIT_ERR_Q = 32;
    static constexpr int SPLIT_DEPTH = 3;
    // Only edges with an end nearer than SPLIT_NEAR halves of H are split:
    // the corners the GTE cannot project, the warp the old near-plane
    // subdivision hid. Each child there is a clipped triangle, and 2H or 4H
    // cost the lamplighter 3 to 8 ms a frame. Farther geometry keeps the GPU's
    // affine mapping and pays a single compare.
    static constexpr int32_t SPLIT_NEAR = 1;
    int32_t m_splitZ = 0;
    // The threshold is one value for a whole frame, so no split is refused
    // halfway through one (which would open a T-junction against a neighbour
    // split earlier). Between frames beginSplitFrame moves it: a frame that
    // made more than SPLIT_BUDGET children raises it by a quarter, up to
    // SPLIT_ERR_Q_MAX; one under half the budget lowers it by an eighth, down
    // to SPLIT_ERR_Q; in between it holds. 1024 keeps it at the base in the
    // courtyard, lamplighter and walk worlds. At 512 it rose to 10 pixels in
    // the courtyard, and at the walk worlds' end it sat at the ceiling while
    // their children fell only a tenth: there most splits are of edges
    // crossing the near plane, which qualify at any threshold.
    static constexpr int32_t SPLIT_BUDGET = 1024;
    static constexpr int32_t SPLIT_ERR_Q_MAX = 8 * SPLIT_ERR_Q;
    int32_t m_splitQ = SPLIT_ERR_Q;
    int32_t m_splitKids = 0;
    void beginSplitFrame();
    int32_t m_projH = 0;

#if PSXSPLASH_FEATURE_SKIN
    void renderSkinnedObjects(eastl::vector<GameObject*>& objects,
                              const psyqo::Vec3& cameraPosition,
                              int32_t fogFarSZ,
                              OT& ot,
                              Balloc& balloc,
                              const Frustum* frustum = nullptr);
#endif
};

}  // namespace psxsplash
