#include "splashpack.hh"

#include <EASTL/vector.h>

#include <psyqo/fixed-point.hh>
#include <psyqo/gte-registers.hh>
#include <psyqo/primitives/common.hh>

#include "bvh.hh"
#include "collision.hh"
#include "gameobject.hh"
#include "cutscene.hh"
#include "lua.h"
#include "mesh.hh"
#include "skinmesh.hh"
#include "streq.hh"
#include "navregion.hh"
#include "memorycardmanager.hh"

namespace psxsplash {

// A pack can carry data for a subsystem this build left out (the exporter and
// the engine build disagree, or the engine was built by hand). That data is
// skipped, and said so, rather than parsed into code that is not there.
[[maybe_unused]] static void skipCompiledOut(unsigned count, const char* what, const char* feature) {
    if (count == 0) return;
    printf("Splashpack: %u %s ignored, feature '%s' is not in this build\n", count, what, feature);
}

struct SPLASHPACKFileHeader {
    char magic[2];
    uint16_t version;
    uint16_t luaFileCount;
    uint16_t gameObjectCount;
    uint16_t textureAtlasCount;
    uint16_t clutCount;
    uint16_t colliderCount;
    uint16_t interactableCount;
    psyqo::GTE::PackedVec3 playerStartPos;
    psyqo::GTE::PackedVec3 playerStartRot;
    psyqo::FixedPoint<12, uint16_t> playerHeight;
    uint16_t sceneLuaFileIndex;
    uint16_t bvhNodeCount;
    uint16_t bvhTriangleRefCount;
    uint16_t sceneType;
    uint16_t triggerBoxCount;
    uint16_t worldCollisionMeshCount;
    uint16_t worldCollisionTriCount;
    uint16_t navRegionCount;
    uint16_t navPortalCount;
    uint16_t moveSpeed;
    uint16_t sprintSpeed;
    uint16_t jumpVelocity;
    uint16_t gravity;
    uint16_t playerRadius;
    uint16_t pad1;
    uint32_t nameTableOffset;
    uint16_t audioClipCount;
    uint16_t pad2;
    uint32_t audioTableOffset;
    uint8_t fogEnabled;
    uint8_t fogR, fogG, fogB;
    uint8_t fogDensity;
    uint8_t pad3;
    uint16_t roomCount;
    uint16_t portalCount;
    uint16_t roomTriRefCount;
    uint16_t cutsceneCount;
    uint16_t roomCellCount;
    uint32_t cutsceneTableOffset;
    uint16_t uiCanvasCount;
    uint8_t  uiFontCount;
    uint8_t  uiPad5;
    uint32_t uiTableOffset;
    uint32_t pixelDataOffset;
    uint16_t animationCount;
    uint16_t roomPortalRefCount;
    uint32_t animationTableOffset;
    uint16_t skinnedMeshCount;
    uint16_t agentCount;
    uint32_t skinTableOffset;
    // --- v21 additions (appended; existing fields above are unchanged) ---
    uint32_t memcardTableOffset;  // offset to SPLASHPACKMemcard, or 0 if none
    uint32_t streamTableOffset;   // offset to SPLASHPACKStreamTable, or 0 if the world is not streamed
    // --- v22 additions (appended; existing fields above are unchanged) ---
    uint32_t spriteTableOffset;   // offset to the sheet+anim tables, or 0 if none
    uint16_t spriteSheetCount;
    uint16_t spriteAnimCount;
    // Authored network scene id (FNV-1a32 of the exporter's SceneNetworkId).
    // 0 means "not authored": the runtime falls back to the derived hash, which
    // is what keeps pre-v22 packs on the network.
    uint32_t sceneHash;
    // --- v23 ---
    // Offset to the tilemap header (SPLASHPACKTilemap), or 0 if none. This is the
    // word that was reservedV22: the header does not grow, and a v22 pack left it
    // 0, so it reads as "no tilemap" without any version-size special case.
    uint32_t tilemapTableOffset;
    // --- v24 (appended; the header grows from 144 to 148) ---
    // Offset to the point light table, or 0 if none.
    uint32_t lightTableOffset;
};
static_assert(sizeof(SPLASHPACKFileHeader) == 148, "SPLASHPACKFileHeader must be 148 bytes");

// Historical header sizes. The header has only ever grown by appending, so an
// older pack is parsed by starting the cursor at the size it had back then.
static constexpr uint32_t kSplashpackHeaderSizeV20 = 120;
static constexpr uint32_t kSplashpackHeaderSizeV21 = 128;
static constexpr uint32_t kSplashpackHeaderSizeV22 = 144;

static uint32_t splashpackHeaderSize(uint16_t version) {
    if (version >= 24) return sizeof(SPLASHPACKFileHeader);
    if (version >= 22) return kSplashpackHeaderSizeV22;
    if (version >= 21) return kSplashpackHeaderSizeV21;
    return kSplashpackHeaderSizeV20;
}

// Memory card save configuration (v21+). Fixed-size so the binary layout is
// trivial to match exactly on both the C# writer and the C++ reader. Region
// and product code build the Sony filename; the title is shown by the BIOS;
// the CLUT + icon frames are the BIOS save icon.
struct SPLASHPACKMemcard {
    char region[2];               // 0x00 e.g. "BA"/"BE"/"BI"
    char product[10];             // 0x02 e.g. "SLUS-00000"
    char title[32];               // 0x0C ASCII, zero-padded
    uint8_t iconFrameCount;       // 0x2C 1..3
    uint8_t mcPad0;               // 0x2D
    uint16_t mcPad1;              // 0x2E alignment
    uint16_t clut[16];            // 0x30 16-colour palette, BGR555
    uint8_t iconPixels[3][128];   // 0x50 up to 3 frames of 16x16 4bpp
};
static_assert(sizeof(SPLASHPACKMemcard) == 464, "SPLASHPACKMemcard must be 464 bytes");

struct SPLASHPACKTextureAtlas {
    uint32_t polygonsOffset;
    uint16_t width, height;
    uint16_t x, y;
};

struct SPLASHPACKClut {
    uint32_t clutOffset;
    uint16_t clutPackingX;
    uint16_t clutPackingY;
    uint16_t length;
    uint16_t pad;
};

void SplashPackLoader::LoadSplashpack(uint8_t *data, SplashpackSceneSetup &setup) {
    psyqo::Kernel::assert(data != nullptr, "Splashpack loading data pointer is null");
    psxsplash::SPLASHPACKFileHeader *header = reinterpret_cast<psxsplash::SPLASHPACKFileHeader *>(data);
    psyqo::Kernel::assert(__builtin_memcmp(header->magic, "SP", 2) == 0, "Splashpack has incorrect magic");
    psyqo::Kernel::assert(header->version >= 20, "Splashpack version too old (need v20+): re-export from SplashEdit");

    setup.playerStartPosition = header->playerStartPos;
    setup.playerStartRotation = header->playerStartRot;
    setup.playerHeight = header->playerHeight;
    
    setup.moveSpeed.value = header->moveSpeed;
    setup.sprintSpeed.value = header->sprintSpeed;
    setup.jumpVelocity.value = header->jumpVelocity;
    setup.gravity.value = header->gravity;
    setup.playerRadius.value = header->playerRadius;

    setup.luaFiles.reserve(header->luaFileCount);
    setup.objects.reserve(header->gameObjectCount);
    setup.colliders.reserve(header->colliderCount);
    setup.interactables.reserve(header->interactableCount);
    // agentCount was pad_skin before v22, so an older pack carries no agents.
    const uint16_t agentCount = header->version >= 22 ? header->agentCount : 0;
    setup.agents.reserve(agentCount);

    uint8_t *cursor = data + splashpackHeaderSize(header->version);

    for (uint16_t i = 0; i < header->luaFileCount; i++) {
        psxsplash::LuaFile *luaHeader = reinterpret_cast<psxsplash::LuaFile *>(cursor);
        luaHeader->luaCode = reinterpret_cast<const char *>(data + luaHeader->luaCodeOffset);
        setup.luaFiles.push_back(luaHeader);
        cursor += sizeof(psxsplash::LuaFile);
    }

    setup.sceneLuaFileIndex = (header->sceneLuaFileIndex == 0xFFFF) ? -1 : (int)header->sceneLuaFileIndex;

    for (uint16_t i = 0; i < header->gameObjectCount; i++) {
        psxsplash::GameObject *go = reinterpret_cast<psxsplash::GameObject *>(cursor);
        go->polygons = reinterpret_cast<psxsplash::Tri *>(data + go->polygonsOffset);
        setup.objects.push_back(go);
        cursor += sizeof(psxsplash::GameObject);
    }

    for (uint16_t i = 0; i < header->colliderCount; i++) {
        psxsplash::SPLASHPACKCollider *collider = reinterpret_cast<psxsplash::SPLASHPACKCollider *>(cursor);
        setup.colliders.push_back(collider);
        cursor += sizeof(psxsplash::SPLASHPACKCollider);
    }

#if !PSXSPLASH_FEATURE_COLLISION
    skipCompiledOut(header->colliderCount, "colliders", "collision");
    skipCompiledOut(header->triggerBoxCount, "trigger boxes", "collision");
#endif
    setup.triggerBoxes.reserve(header->triggerBoxCount);
    for (uint16_t i = 0; i < header->triggerBoxCount; i++) {
        psxsplash::SPLASHPACKTriggerBox *tb = reinterpret_cast<psxsplash::SPLASHPACKTriggerBox *>(cursor);
        setup.triggerBoxes.push_back(tb);
        cursor += sizeof(psxsplash::SPLASHPACKTriggerBox);
    }

    if (header->bvhNodeCount > 0) {
        BVHNode* bvhNodes = reinterpret_cast<BVHNode*>(cursor);
        cursor += header->bvhNodeCount * sizeof(BVHNode);
        
        TriangleRef* triangleRefs = reinterpret_cast<TriangleRef*>(cursor);
        cursor += header->bvhTriangleRefCount * sizeof(TriangleRef);
        
        setup.bvh.initialize(bvhNodes, header->bvhNodeCount, 
                             triangleRefs, header->bvhTriangleRefCount);
    }

    for (uint16_t i = 0; i < header->interactableCount; i++) {
        psxsplash::Interactable *interactable = reinterpret_cast<psxsplash::Interactable *>(cursor);
        setup.interactables.push_back(interactable);
        cursor += sizeof(psxsplash::Interactable);
    }

    for (uint16_t i = 0; i < agentCount; i++) {
        psxsplash::SPLASHPACKAgentV2* agent = reinterpret_cast<psxsplash::SPLASHPACKAgentV2*>(cursor);
        setup.agents.push_back(agent);
        cursor += sizeof(psxsplash::SPLASHPACKAgentV2);
    }
#if !PSXSPLASH_FEATURE_AGENTS
    skipCompiledOut(agentCount, "agents", "agents");
#endif
    // Patrol waypoints (all agents, packed): waypointCount * 3 * 4 bytes per agent
    setup.agentWaypointData = reinterpret_cast<const int32_t*>(cursor);
    {
        uint32_t totalWaypoints = 0;
        for (auto* ag : setup.agents)
            totalWaypoints += ag ? ag->waypointCount : 0;
        cursor += totalWaypoints * 3 * sizeof(int32_t);
    }

    // Skip over legacy world collision data if present in older binaries
    if (header->worldCollisionMeshCount > 0) {
        uintptr_t addr = reinterpret_cast<uintptr_t>(cursor);
        cursor = reinterpret_cast<uint8_t*>((addr + 3) & ~3);
        // CollisionDataHeader: 20 bytes
        const uint16_t meshCount = *reinterpret_cast<const uint16_t*>(cursor);
        const uint16_t triCount = *reinterpret_cast<const uint16_t*>(cursor + 2);
        const uint16_t chunkW = *reinterpret_cast<const uint16_t*>(cursor + 4);
        const uint16_t chunkH = *reinterpret_cast<const uint16_t*>(cursor + 6);
        cursor += 20; // CollisionDataHeader
        cursor += meshCount * 32; // CollisionMeshHeader (32 bytes each)
        cursor += triCount * 52;  // CollisionTri (52 bytes each)
        if (chunkW > 0 && chunkH > 0)
            cursor += chunkW * chunkH * 4; // CollisionChunk (4 bytes each)
    }

    if (header->navRegionCount > 0) {
        uintptr_t addr = reinterpret_cast<uintptr_t>(cursor);
        cursor = reinterpret_cast<uint8_t*>((addr + 3) & ~3);
        // Parsed even without the nav feature: the rooms below follow it.
        cursor = const_cast<uint8_t*>(setup.navRegions.initializeFromData(cursor));
#if !PSXSPLASH_FEATURE_NAV
        skipCompiledOut(header->navRegionCount, "nav regions", "nav");
#endif
    }

    if (header->roomCount > 0) {
        uintptr_t addr = reinterpret_cast<uintptr_t>(cursor);
        cursor = reinterpret_cast<uint8_t*>((addr + 3) & ~3);

        setup.rooms = reinterpret_cast<const RoomData*>(cursor);
        setup.roomCount = header->roomCount;
        cursor += header->roomCount * sizeof(RoomData);

        setup.portals = reinterpret_cast<const PortalData*>(cursor);
        setup.portalCount = header->portalCount;
        cursor += header->portalCount * sizeof(PortalData);

        setup.roomTriRefs = reinterpret_cast<const TriangleRef*>(cursor);
        setup.roomTriRefCount = header->roomTriRefCount;
        cursor += header->roomTriRefCount * sizeof(TriangleRef);

        // Room cells (v17+): per-room spatial subdivision for frustum culling.
        // Cell data follows tri-refs. If roomCellCount is 0, cells == nullptr.
        if (header->roomCellCount > 0) {
            setup.roomCells = reinterpret_cast<const RoomCell*>(cursor);
            setup.roomCellCount = header->roomCellCount;
            cursor += header->roomCellCount * sizeof(RoomCell);
        }

        // Per-room portal reference lists (Phase 5).
        // Each RoomPortalRef is 4 bytes: portalIndex (u16) + otherRoom (u16).
        if (header->roomPortalRefCount > 0) {
            setup.roomPortalRefs = reinterpret_cast<const RoomPortalRef*>(cursor);
            setup.roomPortalRefCount = header->roomPortalRefCount;
            cursor += header->roomPortalRefCount * sizeof(RoomPortalRef);
        }
    }

    // Atlas metadata - v20: pixel data is in a separate .vram file.
    // We still parse the metadata entries (to advance the cursor) since
    // tpage/clut coordinates are baked into the triangle data.
    for (uint16_t i = 0; i < header->textureAtlasCount; i++) {
        cursor += sizeof(psxsplash::SPLASHPACKTextureAtlas);
    }

    // CLUT metadata - v20: CLUT data is in a separate .vram file.
    for (uint16_t i = 0; i < header->clutCount; i++) {
        cursor += sizeof(psxsplash::SPLASHPACKClut);
    }

    if (header->nameTableOffset != 0) {
        uint8_t* nameData = data + header->nameTableOffset;
        setup.objectNames.reserve(header->gameObjectCount);
        for (uint16_t i = 0; i < header->gameObjectCount; i++) {
            uint8_t nameLen = *nameData++;
            const char* nameStr = reinterpret_cast<const char*>(nameData);
            setup.objectNames.push_back(nameStr);
            nameData += nameLen + 1; // +1 for null terminator
        }
    }

    if (header->audioClipCount > 0 && header->audioTableOffset != 0) {
        uint8_t* audioTable = data + header->audioTableOffset;
        setup.audioClips.reserve(header->audioClipCount);
        setup.audioClipNames.reserve(header->audioClipCount);
        for (uint16_t i = 0; i < header->audioClipCount; i++) {
            uint32_t dataOff   = *reinterpret_cast<uint32_t*>(audioTable); audioTable += 4;
            uint32_t size      = *reinterpret_cast<uint32_t*>(audioTable); audioTable += 4;
            uint16_t rate      = *reinterpret_cast<uint16_t*>(audioTable); audioTable += 2;
            uint8_t  loop      = *audioTable++;
            uint8_t  nameLen   = *audioTable++;
            uint32_t nameOff   = *reinterpret_cast<uint32_t*>(audioTable); audioTable += 4;
            SplashpackSceneSetup::AudioClipSetup clip;
            // v20: ADPCM data is in a separate .spu file; dataOff is 0.
            clip.adpcmData = nullptr;
            clip.sizeBytes = size;
            clip.sampleRate = rate;
            clip.loop = (loop != 0);
            clip.name = (nameLen > 0 && nameOff != 0) ? reinterpret_cast<const char*>(data + nameOff) : nullptr;
            setup.audioClips.push_back(clip);
            setup.audioClipNames.push_back(clip.name);
        }
    }

    setup.fogEnabled = header->fogEnabled != 0;
    setup.fogR = header->fogR;
    setup.fogG = header->fogG;
    setup.fogB = header->fogB;
    setup.fogDensity = header->fogDensity;
    setup.sceneType = header->sceneType;

#if !PSXSPLASH_FEATURE_CUTSCENE
    skipCompiledOut(header->cutsceneTableOffset ? header->cutsceneCount : 0, "cutscenes", "cutscene");
    skipCompiledOut(header->animationTableOffset ? header->animationCount : 0, "animations", "cutscene");
#else
    if (header->cutsceneCount > 0 && header->cutsceneTableOffset != 0) {
        setup.cutsceneCount = 0;
        uint8_t* tablePtr = data + header->cutsceneTableOffset;
        int csCount = header->cutsceneCount;
        if (csCount > MAX_CUTSCENES) csCount = MAX_CUTSCENES;

        for (int ci = 0; ci < csCount; ci++) {
            // SPLASHPACKCutsceneEntry: 12 bytes
            uint32_t dataOffset  = *reinterpret_cast<uint32_t*>(tablePtr); tablePtr += 4;
            uint8_t  nameLen     = *tablePtr++;                                       
            tablePtr += 3; // pad
            uint32_t nameOffset  = *reinterpret_cast<uint32_t*>(tablePtr); tablePtr += 4;

            Cutscene& cs = setup.loadedCutscenes[ci];
            cs.name = (nameLen > 0 && nameOffset != 0)
                      ? reinterpret_cast<const char*>(data + nameOffset)
                      : nullptr;

            // SPLASHPACKCutscene: 12 bytes at dataOffset
            uint8_t* csPtr = data + dataOffset;
            cs.totalFrames       = *reinterpret_cast<uint16_t*>(csPtr); csPtr += 2;
            cs.trackCount        = *csPtr++;
            cs.audioEventCount   = *csPtr++;
            uint32_t tracksOff   = *reinterpret_cast<uint32_t*>(csPtr); csPtr += 4;
            uint32_t audioOff    = *reinterpret_cast<uint32_t*>(csPtr); csPtr += 4;

            // v19: skin anim events follow (4 bytes: count + pad + offset)
            cs.skinAnimEventCount = 0;
            cs.skinAnimEvents = nullptr;
            if (header->version >= 19) {
                cs.skinAnimEventCount = *csPtr++;
                csPtr += 3; // pad
                uint32_t skinAnimOff = *reinterpret_cast<uint32_t*>(csPtr); csPtr += 4;
                if (cs.skinAnimEventCount > MAX_SKIN_ANIM_EVENTS)
                    cs.skinAnimEventCount = MAX_SKIN_ANIM_EVENTS;
                cs.skinAnimEvents = (cs.skinAnimEventCount > 0 && skinAnimOff != 0)
                    ? reinterpret_cast<CutsceneSkinAnimEvent*>(data + skinAnimOff)
                    : nullptr;
            }

            if (cs.trackCount > MAX_TRACKS) cs.trackCount = MAX_TRACKS;
            if (cs.audioEventCount > MAX_AUDIO_EVENTS) cs.audioEventCount = MAX_AUDIO_EVENTS;

            // Audio events pointer
            cs.audioEvents = (cs.audioEventCount > 0 && audioOff != 0)
                             ? reinterpret_cast<CutsceneAudioEvent*>(data + audioOff)
                             : nullptr;

            // Parse tracks
            uint8_t* trackPtr = data + tracksOff;
            for (uint8_t ti = 0; ti < cs.trackCount; ti++) {
                CutsceneTrack& track = cs.tracks[ti];

                // SPLASHPACKCutsceneTrack: 12 bytes
                track.trackType     = static_cast<TrackType>(*trackPtr++);
                track.keyframeCount = *trackPtr++;
                uint8_t objNameLen  = *trackPtr++;
                uint8_t lightIndex  = *trackPtr++;  // light tracks only; pad otherwise
                uint32_t objNameOff = *reinterpret_cast<uint32_t*>(trackPtr); trackPtr += 4;
                uint32_t kfOff      = *reinterpret_cast<uint32_t*>(trackPtr); trackPtr += 4;

                // Resolve keyframes pointer
                track.keyframes = (track.keyframeCount > 0 && kfOff != 0)
                                  ? reinterpret_cast<CutsceneKeyframe*>(data + kfOff)
                                  : nullptr;

                // Resolve target object by name (or store UI name for later resolution)
                track.target = nullptr;
                track.uiHandle = isLightTrackType(track.trackType) ? lightIndex : -1;
                if (objNameLen > 0 && objNameOff != 0 && !isLightTrackType(track.trackType)) {
                    const char* objName = reinterpret_cast<const char*>(data + objNameOff);
                    bool isUI = isUITrackType(track.trackType);
                    if (isUI) {
                        // Store the raw name pointer temporarily in target
                        // (will be resolved to uiHandle later by scenemanager)
                        track.target = reinterpret_cast<GameObject*>(const_cast<char*>(objName));
                    } else {
                        for (size_t oi = 0; oi < setup.objectNames.size(); oi++) {
                            if (setup.objectNames[oi] &&
                                streq(setup.objectNames[oi], objName)) {
                                track.target = setup.objects[oi];
                                break;
                            }
                        }
                    }
                    // If not found, target stays nullptr - track will be skipped at runtime
                }
            }

            // Zero out unused track slots
            for (uint8_t ti = cs.trackCount; ti < MAX_TRACKS; ti++) {
                cs.tracks[ti].keyframeCount = 0;
                cs.tracks[ti].keyframes = nullptr;
                cs.tracks[ti].target = nullptr;
                cs.tracks[ti].uiHandle = -1;
                cs.tracks[ti].initialValues[0] = 0;
                cs.tracks[ti].initialValues[1] = 0;
                cs.tracks[ti].initialValues[2] = 0;
            }

            setup.cutsceneCount++;
        }
    }
#endif

#if PSXSPLASH_FEATURE_UI
    if (header->version >= 13) {
        setup.uiCanvasCount = header->uiCanvasCount;
        setup.uiFontCount = header->uiFontCount;
        setup.uiTableOffset = header->uiTableOffset;
    }
#else
    if (header->version >= 13 && header->uiTableOffset != 0)
        skipCompiledOut(header->uiCanvasCount, "UI canvases", "ui");
#endif

#if PSXSPLASH_FEATURE_CUTSCENE
    // Animation loading (v17+)
    if (header->animationCount > 0 && header->animationTableOffset != 0) {
        setup.animationCount = 0;
        uint8_t* tablePtr = data + header->animationTableOffset;
        int anCount = header->animationCount;
        if (anCount > MAX_ANIMATIONS) anCount = MAX_ANIMATIONS;

        for (int ai = 0; ai < anCount; ai++) {
            // SPLASHPACKAnimationEntry: 12 bytes (same layout as cutscene entry)
            uint32_t dataOffset  = *reinterpret_cast<uint32_t*>(tablePtr); tablePtr += 4;
            uint8_t  nameLen     = *tablePtr++;
            tablePtr += 3; // pad
            uint32_t nameOffset  = *reinterpret_cast<uint32_t*>(tablePtr); tablePtr += 4;

            Animation& an = setup.loadedAnimations[ai];
            an.name = (nameLen > 0 && nameOffset != 0)
                      ? reinterpret_cast<const char*>(data + nameOffset)
                      : nullptr;

            // SPLASHPACKAnimation: 8 bytes (no audio), then optionally skin anim events (v19)
            uint8_t* anPtr = data + dataOffset;
            an.totalFrames = *reinterpret_cast<uint16_t*>(anPtr); anPtr += 2;
            an.trackCount  = *anPtr++;
            an.skinAnimEventCount = 0;
            an.skinAnimEvents = nullptr;
            anPtr++; // pad (was 'pad' field)
            uint32_t tracksOff = *reinterpret_cast<uint32_t*>(anPtr); anPtr += 4;

            // v19: skin anim events for animations
            if (header->version >= 19) {
                an.skinAnimEventCount = *anPtr++;
                anPtr += 3; // pad
                uint32_t skinAnimOff = *reinterpret_cast<uint32_t*>(anPtr); anPtr += 4;
                if (an.skinAnimEventCount > MAX_SKIN_ANIM_EVENTS)
                    an.skinAnimEventCount = MAX_SKIN_ANIM_EVENTS;
                an.skinAnimEvents = (an.skinAnimEventCount > 0 && skinAnimOff != 0)
                    ? reinterpret_cast<CutsceneSkinAnimEvent*>(data + skinAnimOff)
                    : nullptr;
            }

            if (an.trackCount > MAX_ANIM_TRACKS) an.trackCount = MAX_ANIM_TRACKS;

            // Parse tracks (same format as cutscene tracks)
            uint8_t* trackPtr = data + tracksOff;
            for (uint8_t ti = 0; ti < an.trackCount; ti++) {
                CutsceneTrack& track = an.tracks[ti];

                track.trackType     = static_cast<TrackType>(*trackPtr++);
                track.keyframeCount = *trackPtr++;
                uint8_t objNameLen  = *trackPtr++;
                uint8_t lightIndex  = *trackPtr++;  // light tracks only; pad otherwise
                uint32_t objNameOff = *reinterpret_cast<uint32_t*>(trackPtr); trackPtr += 4;
                uint32_t kfOff      = *reinterpret_cast<uint32_t*>(trackPtr); trackPtr += 4;

                track.keyframes = (track.keyframeCount > 0 && kfOff != 0)
                                  ? reinterpret_cast<CutsceneKeyframe*>(data + kfOff)
                                  : nullptr;

                track.target = nullptr;
                track.uiHandle = isLightTrackType(track.trackType) ? lightIndex : -1;
                if (objNameLen > 0 && objNameOff != 0 && !isLightTrackType(track.trackType)) {
                    const char* objName = reinterpret_cast<const char*>(data + objNameOff);
                    bool isUI = isUITrackType(track.trackType);
                    if (isUI) {
                        track.target = reinterpret_cast<GameObject*>(const_cast<char*>(objName));
                    } else {
                        for (size_t oi = 0; oi < setup.objectNames.size(); oi++) {
                            if (setup.objectNames[oi] &&
                                streq(setup.objectNames[oi], objName)) {
                                track.target = setup.objects[oi];
                                break;
                            }
                        }
                    }
                }
            }

            // Zero unused track slots
            for (uint8_t ti = an.trackCount; ti < MAX_ANIM_TRACKS; ti++) {
                an.tracks[ti].keyframeCount = 0;
                an.tracks[ti].keyframes = nullptr;
                an.tracks[ti].target = nullptr;
                an.tracks[ti].uiHandle = -1;
                an.tracks[ti].initialValues[0] = 0;
                an.tracks[ti].initialValues[1] = 0;
                an.tracks[ti].initialValues[2] = 0;
            }

            setup.animationCount++;
        }
    }
#endif

    // Skinned mesh loading (v18+)
#if !PSXSPLASH_FEATURE_SKIN
    if (header->version >= 18 && header->skinTableOffset != 0)
        skipCompiledOut(header->skinnedMeshCount, "skinned meshes", "skin");
#else
    if (header->version >= 18 && header->skinnedMeshCount > 0 && header->skinTableOffset != 0) {
        uint8_t* tablePtr = data + header->skinTableOffset;
        int smCount = header->skinnedMeshCount;
        if (smCount > MAX_SKINNED_MESHES) smCount = MAX_SKINNED_MESHES;

        for (int si = 0; si < smCount; si++) {
            uint32_t dataOffset  = *reinterpret_cast<uint32_t*>(tablePtr); tablePtr += 4;
            uint8_t  nameLen     = *tablePtr++;
            tablePtr += 3; // pad
            uint32_t nameOffset  = *reinterpret_cast<uint32_t*>(tablePtr); tablePtr += 4;

            SkinAnimSet& animSet = setup.loadedSkinAnimSets[si];

            // Parse SkinData block
            uint8_t* skinPtr = data + dataOffset;
            animSet.gameObjectIndex = *reinterpret_cast<uint16_t*>(skinPtr); skinPtr += 2;
            animSet.boneCount       = *skinPtr++;
            animSet.clipCount       = *skinPtr++;

            // Bone indices: polyCount x 3 bytes
            uint16_t polyCount = 0;
            if (animSet.gameObjectIndex < setup.objects.size()) {
                polyCount = setup.objects[animSet.gameObjectIndex]->polyCount;
            }
            animSet.boneIndices = skinPtr;
            skinPtr += polyCount * 3;

            // Align to 4-byte boundary
            uintptr_t addr = reinterpret_cast<uintptr_t>(skinPtr);
            skinPtr = reinterpret_cast<uint8_t*>((addr + 3) & ~3);

            // Parse clips
            if (animSet.clipCount > SKINMESH_MAX_CLIPS) animSet.clipCount = SKINMESH_MAX_CLIPS;
            for (uint8_t ci = 0; ci < animSet.clipCount; ci++) {
                SkinAnimClip& clip = animSet.clips[ci];

                uint8_t clipNameLen = *skinPtr++;
                // Null-terminate the name in place
                clip.name = reinterpret_cast<const char*>(skinPtr);
                skinPtr += clipNameLen;
                *skinPtr = '\0';
                skinPtr++;

                clip.flags      = *skinPtr++;
                clip.fps        = *skinPtr++;
                // Align to 2-byte boundary for uint16_t frameCount (MIPS requires aligned reads)
                addr = reinterpret_cast<uintptr_t>(skinPtr);
                skinPtr = reinterpret_cast<uint8_t*>((addr + 1) & ~1);
                clip.frameCount = *reinterpret_cast<uint16_t*>(skinPtr); skinPtr += 2;
                clip.boneCount  = animSet.boneCount;

                // Frame data: frameCount x boneCount x 24 bytes
                clip.frames = reinterpret_cast<const BakedBoneMatrix*>(skinPtr);
                skinPtr += (uint32_t)clip.frameCount * (uint32_t)animSet.boneCount * sizeof(BakedBoneMatrix);
            }

            // Zero unused clip slots
            for (uint8_t ci = animSet.clipCount; ci < SKINMESH_MAX_CLIPS; ci++) {
                animSet.clips[ci].name = nullptr;
                animSet.clips[ci].frames = nullptr;
                animSet.clips[ci].frameCount = 0;
            }

            setup.skinnedMeshCount++;
        }
    }
#endif

    // Memory card save configuration (v21+).
    // Every export writes this table, so there is nothing to report when the
    // memcard feature is out: the config is only read by MemCard.*.
#if PSXSPLASH_FEATURE_MEMCARD
    if (header->version >= 21 && header->memcardTableOffset != 0) {
        const SPLASHPACKMemcard *mc =
            reinterpret_cast<const SPLASHPACKMemcard *>(data + header->memcardTableOffset);

        MemoryCardConfig cfg;
        cfg.valid = true;

        for (int i = 0; i < 2; i++) cfg.region[i] = mc->region[i];
        cfg.region[2] = '\0';
        for (int i = 0; i < 10; i++) cfg.product[i] = mc->product[i];
        cfg.product[10] = '\0';
        cfg.product[11] = '\0';
        for (int i = 0; i < 32; i++) cfg.titlePrefix[i] = mc->title[i];
        cfg.titlePrefix[32] = '\0';

        uint8_t frames = mc->iconFrameCount;
        if (frames < 1) frames = 1;
        if (frames > 3) frames = 3;
        cfg.icon.frameCount = frames;
        for (int i = 0; i < 16; i++) cfg.icon.clut[i] = mc->clut[i];
        for (int f = 0; f < 3; f++) {
            __builtin_memcpy(cfg.icon.pixels[f], mc->iconPixels[f], 128);
        }

        MemoryCardManager::Get().setConfig(cfg);
    }
#endif

    // Sprites and the authored scene hash (v22+). Older packs leave these zero,
    // which the SpriteSystem reads as "no sheets" and NetworkManager reads as
    // "derive the hash the old way".
    if (header->version >= 22) {
#if PSXSPLASH_FEATURE_SPRITES
        setup.spriteSheetCount = header->spriteSheetCount;
        setup.spriteAnimCount = header->spriteAnimCount;
        setup.spriteTableOffset = header->spriteTableOffset;
#else
        skipCompiledOut(header->spriteTableOffset ? header->spriteSheetCount : 0, "sprite sheets", "sprites");
#endif
        setup.sceneHash = header->sceneHash;
    }

    // Tilemap (v23+). The header word is 0 on older packs, which the TileSystem
    // reads as "no map" - so this needs no version gate beyond the field's
    // meaning, but keep the explicit check for symmetry with the block above.
    if (header->version >= 23) {
#if PSXSPLASH_FEATURE_SPRITES
        setup.tilemapTableOffset = header->tilemapTableOffset;
#else
        skipCompiledOut(header->tilemapTableOffset != 0, "tilemap", "sprites");
#endif
    }

    // Point lights (v24+).
    if (header->version >= 24 && header->lightTableOffset != 0) {
        const uint8_t* table = data + header->lightTableOffset;
        uint16_t count = *reinterpret_cast<const uint16_t*>(table);
#if !PSXSPLASH_FEATURE_LIGHTS
        skipCompiledOut(count, "point lights", "lights");
        count = 0;
#endif
        if (count > MAX_SCENE_LIGHTS) count = MAX_SCENE_LIGHTS;
        setup.pointLights = reinterpret_cast<const SPLASHPACKPointLight*>(table + 4);
        setup.pointLightCount = count;
    }

    // Streamed world geometry. This word was reserved and written as 0 since v21.
    if (header->version >= 21) {
#if PSXSPLASH_FEATURE_STREAMING
        setup.streamTableOffset = header->streamTableOffset;
#else
        skipCompiledOut(header->streamTableOffset != 0, "streamed world table", "streaming");
#endif
    }
}

}  // namespace psxsplash