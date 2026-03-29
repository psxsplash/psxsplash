#include "splashpack.hh"

#include <EASTL/vector.h>

#include <psyqo/fixed-point.hh>
#include <psyqo/gte-registers.hh>
#include <psyqo/primitives/common.hh>

#include "bvh.hh"
#include "collision.hh"
#include "gameobject.hh"
#include "lua.h"
#include "mesh.hh"
#include "worldcollision.hh"
#include "navregion.hh"
#include "renderer.hh"

namespace psxsplash {

struct SPLASHPACKFileHeader {
    char magic[2];              // "SP"
    uint16_t version;           // Format version (8 = movement params)
    uint16_t luaFileCount;
    uint16_t gameObjectCount;
    uint16_t navmeshCount;
    uint16_t textureAtlasCount;
    uint16_t clutCount;
    uint16_t colliderCount;
    psyqo::GTE::PackedVec3 playerStartPos;
    psyqo::GTE::PackedVec3 playerStartRot;
    psyqo::FixedPoint<12, uint16_t> playerHeight;
    uint16_t sceneLuaFileIndex;
    // Version 3 additions:
    uint16_t bvhNodeCount;
    uint16_t bvhTriangleRefCount;
    // Version 4 additions (component counts):
    uint16_t interactableCount;
    uint16_t healthCount;
    uint16_t timerCount;
    uint16_t spawnerCount;
    // Version 5 additions (navgrid):
    uint16_t hasNavGrid;        // 1 if navgrid present, 0 otherwise
    uint16_t reserved;          // Alignment padding
    // Version 6 additions (AABB + scene type):
    uint16_t sceneType;         // 0 = exterior, 1 = interior
    uint16_t reserved2;         // Alignment padding
    // Version 7 additions (world collision + nav regions):
    uint16_t worldCollisionMeshCount;
    uint16_t worldCollisionTriCount;
    uint16_t navRegionCount;
    uint16_t navPortalCount;
    // Version 8 additions (movement parameters):
    uint16_t moveSpeed;         // fp12 per-frame speed constant
    uint16_t sprintSpeed;       // fp12 per-frame speed constant
    uint16_t jumpVelocity;      // fp12 per-second initial jump velocity
    uint16_t gravity;           // fp12 per-second² downward acceleration
    uint16_t playerRadius;      // fp12 collision radius
    uint16_t reserved3;         // Alignment padding
    // Version 9 additions (object names):
    uint32_t nameTableOffset;   // Offset to name string table (0 = no names)
    // Version 10 additions (audio):
    uint16_t audioClipCount;    // Number of audio clips
    uint16_t reserved4;         // Alignment padding
    uint32_t audioTableOffset;  // Offset to audio clip table (0 = no audio)
    // Version 11 additions (fog + room/portal):
    uint8_t fogEnabled;         // 0 = off, 1 = on
    uint8_t fogR, fogG, fogB;   // Fog color RGB
    uint8_t fogDensity;         // 1-10 density scale
    uint8_t reserved5;          // Alignment
    uint16_t roomCount;         // 0 = no room system (use BVH path)
    uint16_t portalCount;
    uint16_t roomTriRefCount;
};
static_assert(sizeof(SPLASHPACKFileHeader) == 96, "SPLASHPACKFileHeader must be 96 bytes");

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
    psyqo::Kernel::assert(header->version >= 8, "Splashpack version mismatch: re-export from SplashEdit");

    setup.playerStartPosition = header->playerStartPos;
    setup.playerStartRotation = header->playerStartRot;
    setup.playerHeight = header->playerHeight;
    
    // Movement parameters (v8+)
    setup.moveSpeed.value = header->moveSpeed;
    setup.sprintSpeed.value = header->sprintSpeed;
    setup.jumpVelocity.value = header->jumpVelocity;
    setup.gravity.value = header->gravity;
    setup.playerRadius.value = header->playerRadius;

    setup.luaFiles.reserve(header->luaFileCount);
    setup.objects.reserve(header->gameObjectCount);
    setup.colliders.reserve(header->colliderCount);
    
    // Reserve component arrays (version 4+)
    if (header->version >= 4) {
        setup.interactables.reserve(header->interactableCount);
    }

    // V10 header = 84 bytes, V11+ = 96 bytes. sizeof() always returns 96,
    // so we must compute the correct offset for older versions.
    uint32_t headerSize = (header->version >= 11) ? 96 : 84;
    uint8_t *cursor = data + headerSize;

    for (uint16_t i = 0; i < header->luaFileCount; i++) {
        psxsplash::LuaFile *luaHeader = reinterpret_cast<psxsplash::LuaFile *>(cursor);
        luaHeader->luaCode = reinterpret_cast<const char *>(data + luaHeader->luaCodeOffset);
        setup.luaFiles.push_back(luaHeader);
        cursor += sizeof(psxsplash::LuaFile);
    }

    // sceneLuaFileIndex is stored as uint16_t in header; 0xFFFF means "no scene script" (-1)
    setup.sceneLuaFileIndex = (header->sceneLuaFileIndex == 0xFFFF) ? -1 : (int)header->sceneLuaFileIndex;

    for (uint16_t i = 0; i < header->gameObjectCount; i++) {
        psxsplash::GameObject *go = reinterpret_cast<psxsplash::GameObject *>(cursor);
        go->polygons = reinterpret_cast<psxsplash::Tri *>(data + go->polygonsOffset);
        setup.objects.push_back(go);
        cursor += sizeof(psxsplash::GameObject);
    }

    // Read collision data (after GameObjects)
    for (uint16_t i = 0; i < header->colliderCount; i++) {
        psxsplash::SPLASHPACKCollider *collider = reinterpret_cast<psxsplash::SPLASHPACKCollider *>(cursor);
        setup.colliders.push_back(collider);
        cursor += sizeof(psxsplash::SPLASHPACKCollider);
    }

    // Read BVH data (version 3+)
    if (header->version >= 3 && header->bvhNodeCount > 0) {
        BVHNode* bvhNodes = reinterpret_cast<BVHNode*>(cursor);
        cursor += header->bvhNodeCount * sizeof(BVHNode);
        
        TriangleRef* triangleRefs = reinterpret_cast<TriangleRef*>(cursor);
        cursor += header->bvhTriangleRefCount * sizeof(TriangleRef);
        
        setup.bvh.initialize(bvhNodes, header->bvhNodeCount, 
                             triangleRefs, header->bvhTriangleRefCount);
    }
    
    // Read component data (version 4+)
    if (header->version >= 4) {
        // Interactables
        for (uint16_t i = 0; i < header->interactableCount; i++) {
            psxsplash::Interactable *interactable = reinterpret_cast<psxsplash::Interactable *>(cursor);
            setup.interactables.push_back(interactable);
            cursor += sizeof(psxsplash::Interactable);
        }
        
        // Skip health components (legacy, 24 bytes each)
        cursor += header->healthCount * 24;
        
        // Skip timers (legacy, 16 bytes each)
        cursor += header->timerCount * 16;
        
        // Skip spawners (legacy, 44 bytes each)
        cursor += header->spawnerCount * 44;
    }
    
    // Read NavGrid (version 5+ — LEGACY, skip if present)
    if (header->version >= 5 && header->hasNavGrid) {
        // Skip NavGrid data: header (16 bytes) + cells
        // NavGridHeader: 4 int32 = 16 bytes, then gridW*gridH*9 bytes
        int32_t* navGridHeader = reinterpret_cast<int32_t*>(cursor);
        int32_t gridW = navGridHeader[2];
        int32_t gridH = navGridHeader[3];
        cursor += 16; // header
        cursor += gridW * gridH * 9; // cells (9 bytes each)
        // Align to 4 bytes
        uintptr_t addr = reinterpret_cast<uintptr_t>(cursor);
        cursor = reinterpret_cast<uint8_t*>((addr + 3) & ~3);
    }

    // Read world collision soup (version 7+)
    if (header->version >= 7 && header->worldCollisionMeshCount > 0) {
        uintptr_t addr = reinterpret_cast<uintptr_t>(cursor);
        cursor = reinterpret_cast<uint8_t*>((addr + 3) & ~3);
        cursor = const_cast<uint8_t*>(setup.worldCollision.initializeFromData(cursor));
    }

    // Read nav regions (version 7+)
    if (header->version >= 7 && header->navRegionCount > 0) {
        uintptr_t addr = reinterpret_cast<uintptr_t>(cursor);
        cursor = reinterpret_cast<uint8_t*>((addr + 3) & ~3);
        cursor = const_cast<uint8_t*>(setup.navRegions.initializeFromData(cursor));
    }

    // Read room/portal data (version 11+, interior scenes)
    // Must be read here (after nav regions, before navmesh skip / atlas metadata)
    // to match the sequential cursor position where the writer places it.
    if (header->version >= 11 && header->roomCount > 0) {
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
    }

    // Skip legacy navmesh metadata (still present in v7 files)
    cursor += header->navmeshCount * 8; // Navmesh struct: 4+2+2 = 8 bytes

    for (uint16_t i = 0; i < header->textureAtlasCount; i++) {
        psxsplash::SPLASHPACKTextureAtlas *atlas = reinterpret_cast<psxsplash::SPLASHPACKTextureAtlas *>(cursor);
        uint8_t *offsetData = data + atlas->polygonsOffset;
        uint16_t *castedData = reinterpret_cast<uint16_t *>(offsetData);
        psxsplash::Renderer::GetInstance().VramUpload(castedData, atlas->x, atlas->y, atlas->width, atlas->height);
        cursor += sizeof(psxsplash::SPLASHPACKTextureAtlas);
    }

    for (uint16_t i = 0; i < header->clutCount; i++) {
        psxsplash::SPLASHPACKClut *clut = reinterpret_cast<psxsplash::SPLASHPACKClut *>(cursor);
        uint8_t *clutOffset = data + clut->clutOffset;
        psxsplash::Renderer::GetInstance().VramUpload((uint16_t *)clutOffset, clut->clutPackingX * 16,
                                                      clut->clutPackingY, clut->length, 1);
        cursor += sizeof(psxsplash::SPLASHPACKClut);
    }

    // Read object name table (version 9+)
    if (header->version >= 9 && header->nameTableOffset != 0) {
        uint8_t* nameData = data + header->nameTableOffset;
        setup.objectNames.reserve(header->gameObjectCount);
        for (uint16_t i = 0; i < header->gameObjectCount; i++) {
            uint8_t nameLen = *nameData++;
            const char* nameStr = reinterpret_cast<const char*>(nameData);
            // Names are stored as length-prefixed, null-terminated strings
            setup.objectNames.push_back(nameStr);
            nameData += nameLen + 1; // +1 for null terminator
        }
    }

    // Read audio clip table (version 10+)
    if (header->version >= 10 && header->audioClipCount > 0 && header->audioTableOffset != 0) {
        // Audio table: per clip: uint32_t dataOffset, uint32_t sizeBytes, uint16_t sampleRate, uint8_t loop, uint8_t nameLen, uint32_t nameOffset
        // Total 16 bytes per entry
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
            clip.adpcmData = data + dataOff;
            clip.sizeBytes = size;
            clip.sampleRate = rate;
            clip.loop = (loop != 0);
            clip.name = (nameLen > 0 && nameOff != 0) ? reinterpret_cast<const char*>(data + nameOff) : nullptr;
            setup.audioClips.push_back(clip);
            setup.audioClipNames.push_back(clip.name);
        }
    }

    // Read fog configuration (version 11+)
    if (header->version >= 11) {
        setup.fogEnabled = header->fogEnabled != 0;
        setup.fogR = header->fogR;
        setup.fogG = header->fogG;
        setup.fogB = header->fogB;
        setup.fogDensity = header->fogDensity;
    }

    // Read scene type (version 6+ stored it but it was never read until now)
    setup.sceneType = header->sceneType;

}

}  // namespace psxsplash
