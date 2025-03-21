#include "splashpack.hh"
#include "gameobject.hh"
#include "mesh.hh"
#include "renderer.hh"
#include <cassert>
#include <cstdint>
#include <cstring>

eastl::vector<psxsplash::GameObject*> LoadSplashpack(uint8_t *data) {
    assert(data != nullptr);
    psxsplash::SPLASHPACKFileHeader *header =
        reinterpret_cast<psxsplash::SPLASHPACKFileHeader *>(data);
    assert(memcmp(header->magic, "SP", 10) == 0);

    eastl::vector<psxsplash::GameObject*> gameObjects;
    gameObjects.reserve(header->gameObjectCount);

    uint8_t* curentPointer = data += sizeof(psxsplash::SPLASHPACKFileHeader);

    for(uint16_t i = 0; i < header->gameObjectCount; i++) {
        psxsplash::GameObject* go = reinterpret_cast<psxsplash::GameObject*>(curentPointer);
        go->polygons = reinterpret_cast<psxsplash::Tri*>(data += go->polygonsOffset);
        gameObjects.push_back(go);
        curentPointer += sizeof(psxsplash::GameObject);
    }

    for(uint16_t i = 0; i < header->textureAtlasCount; i++) {
        psxsplash::SPLASHPACKTextureAtlas* atlas = reinterpret_cast<psxsplash::SPLASHPACKTextureAtlas *>(curentPointer);
        psxsplash::Renderer::getInstance().vramUpload((uint16_t*) (data += atlas->polygonsOffset), atlas->x, atlas->y, atlas->width, atlas->height);
    }

    return gameObjects;
}
