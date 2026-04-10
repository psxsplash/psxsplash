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

namespace psxsplash {

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
    uint16_t pad_skin;
    uint32_t skinTableOffset;
};
static_assert(sizeof(SPLASHPACKFileHeader) == 120, "SPLASHPACKFileHeader must be 120 bytes");

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

    uint8_t *cursor = data + sizeof(SPLASHPACKFileHeader);

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
        cursor = const_cast<uint8_t*>(setup.navRegions.initializeFromData(cursor));
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

    // Atlas metadata — v20: pixel data is in a separate .vram file.
    // We still parse the metadata entries (to advance the cursor) since
    // tpage/clut coordinates are baked into the triangle data.
    for (uint16_t i = 0; i < header->textureAtlasCount; i++) {
        cursor += sizeof(psxsplash::SPLASHPACKTextureAtlas);
    }

    // CLUT metadata — v20: CLUT data is in a separate .vram file.
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
                trackPtr++; // pad
                uint32_t objNameOff = *reinterpret_cast<uint32_t*>(trackPtr); trackPtr += 4;
                uint32_t kfOff      = *reinterpret_cast<uint32_t*>(trackPtr); trackPtr += 4;

                // Resolve keyframes pointer
                track.keyframes = (track.keyframeCount > 0 && kfOff != 0)
                                  ? reinterpret_cast<CutsceneKeyframe*>(data + kfOff)
                                  : nullptr;

                // Resolve target object by name (or store UI name for later resolution)
                track.target = nullptr;
                track.uiHandle = -1;
                if (objNameLen > 0 && objNameOff != 0) {
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
                    // If not found, target stays nullptr — track will be skipped at runtime
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

    if (header->version >= 13) {
        setup.uiCanvasCount = header->uiCanvasCount;
        setup.uiFontCount = header->uiFontCount;
        setup.uiTableOffset = header->uiTableOffset;
    }

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
                trackPtr++; // pad
                uint32_t objNameOff = *reinterpret_cast<uint32_t*>(trackPtr); trackPtr += 4;
                uint32_t kfOff      = *reinterpret_cast<uint32_t*>(trackPtr); trackPtr += 4;

                track.keyframes = (track.keyframeCount > 0 && kfOff != 0)
                                  ? reinterpret_cast<CutsceneKeyframe*>(data + kfOff)
                                  : nullptr;

                track.target = nullptr;
                track.uiHandle = -1;
                if (objNameLen > 0 && objNameOff != 0) {
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

    // Skinned mesh loading (v18+)
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

            // Bone indices: polyCount × 3 bytes
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

                // Frame data: frameCount × boneCount × 24 bytes
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

}

}  // namespace psxsplash