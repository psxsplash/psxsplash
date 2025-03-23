#include "splashpack.hh"
#include "gameobject.hh"
#include "mesh.hh"
#include "psyqo/primitives/common.hh"
#include "renderer.hh"
#include <cstdint>
#include <cstring>
#include <memory>

namespace psxsplash {

eastl::vector<psxsplash::GameObject *> LoadSplashpack(uint8_t *data) {
    psyqo::Kernel::assert(data != nullptr,
                          "Splashpack loading data pointer is null");
    psxsplash::SPLASHPACKFileHeader *header =
        reinterpret_cast<psxsplash::SPLASHPACKFileHeader *>(data);
    psyqo::Kernel::assert(memcmp(header->magic, "SP", 2) == 0,
                          "Splashpack has incorrect magic");

    eastl::vector<psxsplash::GameObject *> gameObjects;
    gameObjects.reserve(header->gameObjectCount);

    uint8_t *curentPointer = data + sizeof(psxsplash::SPLASHPACKFileHeader);

    for (uint16_t i = 0; i < header->gameObjectCount; i++) {
        psxsplash::GameObject *go =
            reinterpret_cast<psxsplash::GameObject *>(curentPointer);

        int16_t width = 0;
        switch ((psyqo::Prim::TPageAttr::ColorMode)(
            (std::bit_cast<short>(go->texture) & 0x0180) >> 7)) {
        case psyqo::Prim::TPageAttr::ColorMode::Tex4Bits:
            width = 16;
            break;
        case psyqo::Prim::TPageAttr::ColorMode::Tex8Bits:
            width = 256;
            break;
        default:
            width = -1;
        }

        if(width > 0) {
            psxsplash::Renderer::getInstance().vramUpload(
                go->clut, go->clutX*16, go->clutY, width, 1);
        }

        go->polygons =
            reinterpret_cast<psxsplash::Tri *>(data + go->polygonsOffset);

        gameObjects.push_back(go);
        curentPointer += sizeof(psxsplash::GameObject);
    }

    for (uint16_t i = 0; i < header->textureAtlasCount; i++) {
        psxsplash::SPLASHPACKTextureAtlas *atlas =
            reinterpret_cast<psxsplash::SPLASHPACKTextureAtlas *>(
                curentPointer);

        uint8_t *offsetData =
            data + atlas->polygonsOffset; // Ensure correct byte offset
        uint16_t *castedData =
            reinterpret_cast<uint16_t *>(offsetData); // Safe cast
        psxsplash::Renderer::getInstance().vramUpload(
            castedData, atlas->x, atlas->y, atlas->width, atlas->height);
        curentPointer += sizeof(psxsplash::SPLASHPACKTextureAtlas);
    }

    return gameObjects;
}

} // namespace psxsplash
