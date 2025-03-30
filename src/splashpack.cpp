#include "splashpack.hh"

#include <cstdint>
#include <cstring>
#include <psyqo/primitives/common.hh>

#include "gameobject.hh"
#include "mesh.hh"
#include "renderer.hh"

namespace psxsplash {

eastl::vector<psxsplash::GameObject *> LoadSplashpack(uint8_t *data) {
    psyqo::Kernel::assert(data != nullptr, "Splashpack loading data pointer is null");
    psxsplash::SPLASHPACKFileHeader *header = reinterpret_cast<psxsplash::SPLASHPACKFileHeader *>(data);
    psyqo::Kernel::assert(memcmp(header->magic, "SP", 2) == 0, "Splashpack has incorrect magic");

    eastl::vector<psxsplash::GameObject *> gameObjects;
    gameObjects.reserve(header->gameObjectCount);

    uint8_t *curentPointer = data + sizeof(psxsplash::SPLASHPACKFileHeader);

    for (uint16_t i = 0; i < header->gameObjectCount; i++) {
        psxsplash::GameObject *go = reinterpret_cast<psxsplash::GameObject *>(curentPointer);
        go->polygons = reinterpret_cast<psxsplash::Tri *>(data + go->polygonsOffset);
        gameObjects.push_back(go);
        curentPointer += sizeof(psxsplash::GameObject);
    }

    for (uint16_t i = 0; i < header->textureAtlasCount; i++) {
        psxsplash::SPLASHPACKTextureAtlas *atlas = reinterpret_cast<psxsplash::SPLASHPACKTextureAtlas *>(curentPointer);

        uint8_t *offsetData = data + atlas->polygonsOffset;               
        uint16_t *castedData = reinterpret_cast<uint16_t *>(offsetData);
        psxsplash::Renderer::getInstance().vramUpload(castedData, atlas->x, atlas->y, atlas->width, atlas->height);
        curentPointer += sizeof(psxsplash::SPLASHPACKTextureAtlas);
    }

    for (uint16_t i = 0; i < header->clutCount; i++) {
        psxsplash::SPLASHPACKClut *clut = reinterpret_cast<psxsplash::SPLASHPACKClut *>(curentPointer);
        psxsplash::Renderer::getInstance().vramUpload(clut->clut, clut->clutPackingX * 16, clut->clutPackingY, clut->length, 1);
        curentPointer += sizeof(psxsplash::SPLASHPACKClut);
    }

    return gameObjects;
}

}  // namespace psxsplash
