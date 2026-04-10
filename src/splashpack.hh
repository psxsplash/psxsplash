#pragma once

#include <EASTL/vector.h>

#include <psyqo/fixed-point.hh>

#include "bvh.hh"
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

struct SplashpackSceneSetup {
    int sceneLuaFileIndex;
    eastl::vector<LuaFile *> luaFiles;
    eastl::vector<GameObject *> objects;
    eastl::vector<SPLASHPACKCollider *> colliders;
    eastl::vector<SPLASHPACKTriggerBox *> triggerBoxes;

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
    psyqo::FixedPoint<12, uint16_t> gravity;          // Per-second² acceleration (fp12)
    psyqo::FixedPoint<12, uint16_t> playerRadius;    // Collision radius (fp12)

    Cutscene loadedCutscenes[MAX_CUTSCENES];
    int cutsceneCount = 0;

    Animation loadedAnimations[MAX_ANIMATIONS];
    int animationCount = 0;

    SkinAnimSet loadedSkinAnimSets[MAX_SKINNED_MESHES];
    int skinnedMeshCount = 0;

    uint16_t uiCanvasCount = 0;
    uint8_t  uiFontCount = 0;
    uint32_t uiTableOffset = 0;
};

class SplashPackLoader {
  public:
    void LoadSplashpack(uint8_t *data, SplashpackSceneSetup &setup);
};

}  // namespace psxsplash
