#pragma once

#include <EASTL/vector.h>

#include <psyqo/fixed-point.hh>

#include "bvh.hh"
#include "features.hh"
#include "collision.hh"
#include "gameobject.hh"
#include "lua.h"
#include "navregion.hh"
#include "audiomanager.hh"
#include "interactable.hh"
#include "cutscene.hh"
#include "animation.hh"
#include "skinmesh.hh"
#include "uisystem.hh"
#include "lightmath.hh"

namespace psxsplash {

/**
 * Collision data as stored in the binary file (fixed layout for serialization)
 */
struct SPLASHPACKCollider {
    // AABB bounds in fixed-point (24 bytes)
    int32_t minX, minY, minZ;
    int32_t maxX, maxY, maxZ;
    // Collision metadata (8 bytes)
    uint8_t collisionType;
    uint8_t layerMask;
    uint16_t gameObjectIndex;
    uint32_t padding;
};
static_assert(sizeof(SPLASHPACKCollider) == 32, "SPLASHPACKCollider must be 32 bytes");

struct SPLASHPACKTriggerBox {
    int32_t minX, minY, minZ;
    int32_t maxX, maxY, maxZ;
    int16_t luaFileIndex;
    uint16_t padding;
    uint32_t padding2;
};
static_assert(sizeof(SPLASHPACKTriggerBox) == 32, "SPLASHPACKTriggerBox must be 32 bytes");

/**
 * Agent configuration stored in the splashpack binary.  Replaces the old
 * 8-byte stub with a complete description of movement, vision, hearing,
 * patrol behaviour and per-state animation mappings.
 *
 * Binary layout (28 bytes) is immediately followed by waypointCount x 12-byte
 * Vec3 patrol waypoints stored in fixed-point 20.12 XYZ order.
 *
 * flags bits:
 *   bit 0  - start enabled
 *   bit 1  - vision enabled
 *   bit 2  - hearing enabled
 *   bit 3  - patrol enabled (loops through waypoints when idle)
 */
struct SPLASHPACKAgentV2 {
    uint16_t gameObjectIndex;   ///< Index into game-object array
    uint8_t  flags;             ///< Behaviour flags (see above)
    uint8_t  waypointCount;     ///< Number of 12-byte Vec3 waypoints that follow
    uint16_t moveSpeed;         ///< Per-frame movement speed (fp12)
    uint16_t stopDistance;      ///< Stop-distance to target (fp12)
    uint16_t visionRange;       ///< Vision range (fp12); 0 = no vision
    int16_t  visionCosAngle;    ///< Half-FOV cosine threshold (fp12, -4096..4096)
    uint16_t hearingRange;      ///< Hearing range (fp12); 0 = no hearing
    uint16_t alertTimeout;      ///< Frames before "target lost" fires after LOS break
    uint8_t  stateAnimClip[8];  ///< Skinned-mesh clip index per AgentState (0xFF = none)
    uint8_t  visionRegionDepth; ///< Max nav-region hops for LOS check (0 = same region only)
    uint8_t  reserved[3];
};
static_assert(sizeof(SPLASHPACKAgentV2) == 28, "SPLASHPACKAgentV2 must be 28 bytes");

/**
 * Point light as stored in the splashpack (v24). The light table is a uint16
 * count and a uint16 pad, then `count` of these.
 */
struct SPLASHPACKPointLight {
    int32_t x, y, z;      ///< world position, 20.12
    int32_t radius;       ///< 20.12; the light reaches zero here
    uint16_t intensity;   ///< 4.12, 4096 = 1.0
    uint8_t r, g, b;
    uint8_t flags;        ///< bit 0 = enabled at load
    uint16_t pad;
    uint32_t nameOffset;  ///< null-terminated name, or 0
};
static_assert(sizeof(SPLASHPACKPointLight) == 28, "SPLASHPACKPointLight must be 28 bytes");

// Legacy alias kept so any old code that still uses SPLASHPACKAgent compiles.
using SPLASHPACKAgent = SPLASHPACKAgentV2;

struct SplashpackSceneSetup {
    int sceneLuaFileIndex;
    eastl::vector<LuaFile *> luaFiles;
    eastl::vector<GameObject *> objects;
    eastl::vector<SPLASHPACKCollider *> colliders;
    eastl::vector<SPLASHPACKTriggerBox *> triggerBoxes;
    eastl::vector<SPLASHPACKAgentV2 *> agents;
    /// Packed fp12 XYZ waypoints for all agents: agent[0]'s waypoints first,
    /// then agent[1]'s, etc.  Points into splashpack data; may be nullptr.
    const int32_t* agentWaypointData = nullptr;

    // New component arrays
    eastl::vector<Interactable *> interactables;

    eastl::vector<const char *> objectNames;

    // Audio clips (v10+): ADPCM data with metadata
    struct AudioClipSetup {
        const uint8_t* adpcmData;
        uint32_t sizeBytes;
        uint16_t sampleRate;
        bool loop;
        const char* name;   // Points into splashpack data (null-terminated)
    };
    eastl::vector<AudioClipSetup> audioClips;

    eastl::vector<const char*> audioClipNames;

    BVHManager bvh;  // Spatial acceleration structure for culling
    NavRegionSystem navRegions;    
    psyqo::GTE::PackedVec3 playerStartPosition;
    psyqo::GTE::PackedVec3 playerStartRotation;
    psyqo::FixedPoint<12, uint16_t> playerHeight;

    // Scene type: 0=exterior (BVH culling), 1=interior (room/portal culling)
    uint16_t sceneType = 0;

    // Fog configuration (v11+)
    bool fogEnabled = false;
    uint8_t fogR = 0, fogG = 0, fogB = 0;
    uint8_t fogDensity = 5;

    const RoomData* rooms = nullptr;
    uint16_t roomCount = 0;
    const PortalData* portals = nullptr;
    uint16_t portalCount = 0;
    const TriangleRef* roomTriRefs = nullptr;
    uint16_t roomTriRefCount = 0;
    const RoomCell* roomCells = nullptr;
    uint16_t roomCellCount = 0;
    const RoomPortalRef* roomPortalRefs = nullptr;
    uint16_t roomPortalRefCount = 0;

    psyqo::FixedPoint<12, uint16_t> moveSpeed;       // Per-frame speed constant (fp12)
    psyqo::FixedPoint<12, uint16_t> sprintSpeed;     // Per-frame sprint constant (fp12)
    psyqo::FixedPoint<12, uint16_t> jumpVelocity;    // Per-second initial velocity (fp12)
    psyqo::FixedPoint<12, uint16_t> gravity;          // Per-second^2 acceleration (fp12)
    psyqo::FixedPoint<12, uint16_t> playerRadius;    // Collision radius (fp12)

#if PSXSPLASH_FEATURE_CUTSCENE
    Cutscene loadedCutscenes[MAX_CUTSCENES];
    Animation loadedAnimations[MAX_ANIMATIONS];
#endif
    int cutsceneCount = 0;
    int animationCount = 0;

#if PSXSPLASH_FEATURE_SKIN
    SkinAnimSet loadedSkinAnimSets[MAX_SKINNED_MESHES];
#endif
    int skinnedMeshCount = 0;

    uint16_t uiCanvasCount = 0;
    uint8_t  uiFontCount = 0;
    uint32_t uiTableOffset = 0;

    // --- v22 ---
    uint16_t spriteSheetCount = 0;
    uint16_t spriteAnimCount = 0;
    uint32_t spriteTableOffset = 0;
    /// Authored network scene id. 0 means "not authored" - the caller falls back
    /// to the derived hash, which is why older packs keep working.
    uint32_t sceneHash = 0;

    // --- v23 ---
    /// Offset to the tilemap header (SPLASHPACKTilemap), or 0 if the scene has no
    /// tilemap. Reuses the v22 reserved header word, so the header size is
    /// unchanged and a v22 pack (which left that word 0) reads as "no tilemap".
    uint32_t tilemapTableOffset = 0;

    // --- v24 ---
    /// Points into splashpack data; count 0 when the scene has no point lights.
    const SPLASHPACKPointLight* pointLights = nullptr;
    uint16_t pointLightCount = 0;
    /// Offset to the streamed-geometry table (SPLASHPACKStreamTable), or 0.
    uint32_t streamTableOffset = 0;
};

class SplashPackLoader {
  public:
    void LoadSplashpack(uint8_t *data, SplashpackSceneSetup &setup);
};

}  // namespace psxsplash
