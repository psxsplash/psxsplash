#include "splashpack.hh"

#include <EASTL/vector.h>

#include <psyqo/fixed-point.hh>
#include <psyqo/gte-registers.hh>
#include <psyqo/primitives/common.hh>

#include "gameobject.hh"
#include "lua.h"
#include "mesh.hh"
#include "navmesh.hh"
#include "renderer.hh"

namespace psxsplash {

struct SPLASHPACKFileHeader {
    char magic[2];
    uint16_t version;
    uint16_t luaFileCount;
    uint16_t gameObjectCount;
    uint16_t navmeshCount;
    uint16_t textureAtlasCount;
    uint16_t clutCount;
    psyqo::GTE::PackedVec3 playerStartPos;
    psyqo::GTE::PackedVec3 playerStartRot;
    psyqo::FixedPoint<12, uint16_t> playerHeight;
    uint16_t sceneLuaFileIndex;
    uint16_t pad;
};

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

    setup.playerStartPosition = header->playerStartPos;
    setup.playerStartRotation = header->playerStartRot;
    setup.playerHeight = header->playerHeight;

    setup.luaFiles.reserve(header->luaFileCount);
    setup.objects.reserve(header->gameObjectCount);
    setup.navmeshes.reserve(header->navmeshCount);

    uint8_t *curentPointer = data + sizeof(psxsplash::SPLASHPACKFileHeader);

    for (uint16_t i = 0; i < header->luaFileCount; i++) {
        psxsplash::LuaFile *luaHeader = reinterpret_cast<psxsplash::LuaFile *>(curentPointer);
        luaHeader->luaCode = reinterpret_cast<const char *>(data + luaHeader->luaCodeOffset);
        setup.luaFiles.push_back(luaHeader);
        curentPointer += sizeof(psxsplash::LuaFile);
    }

    setup.sceneLuaFile = setup.luaFiles[header->sceneLuaFileIndex];

    for (uint16_t i = 0; i < header->gameObjectCount; i++) {
        psxsplash::GameObject *go = reinterpret_cast<psxsplash::GameObject *>(curentPointer);
        go->polygons = reinterpret_cast<psxsplash::Tri *>(data + go->polygonsOffset);
        setup.objects.push_back(go);
        curentPointer += sizeof(psxsplash::GameObject);
    }

    for (uint16_t i = 0; i < header->navmeshCount; i++) {
        psxsplash::Navmesh *navmesh = reinterpret_cast<psxsplash::Navmesh *>(curentPointer);
        navmesh->polygons = reinterpret_cast<psxsplash::NavMeshTri *>(data + navmesh->polygonsOffset);
        setup.navmeshes.push_back(navmesh);
        curentPointer += sizeof(psxsplash::Navmesh);
    }

    for (uint16_t i = 0; i < header->textureAtlasCount; i++) {
        psxsplash::SPLASHPACKTextureAtlas *atlas = reinterpret_cast<psxsplash::SPLASHPACKTextureAtlas *>(curentPointer);
        uint8_t *offsetData = data + atlas->polygonsOffset;
        uint16_t *castedData = reinterpret_cast<uint16_t *>(offsetData);
        psxsplash::Renderer::GetInstance().VramUpload(castedData, atlas->x, atlas->y, atlas->width, atlas->height);
        curentPointer += sizeof(psxsplash::SPLASHPACKTextureAtlas);
    }

    for (uint16_t i = 0; i < header->clutCount; i++) {
        psxsplash::SPLASHPACKClut *clut = reinterpret_cast<psxsplash::SPLASHPACKClut *>(curentPointer);
        uint8_t *clutOffset = data + clut->clutOffset;
        psxsplash::Renderer::GetInstance().VramUpload((uint16_t *)clutOffset, clut->clutPackingX * 16,
                                                      clut->clutPackingY, clut->length, 1);
        curentPointer += sizeof(psxsplash::SPLASHPACKClut);
    }
}

}  // namespace psxsplash
