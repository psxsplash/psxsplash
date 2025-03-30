#pragma once

#include <EASTL/vector.h>

#include <cstdint>

#include "gameobject.hh"

namespace psxsplash {

struct SPLASHPACKFileHeader {
    char magic[2];
    uint16_t version;
    uint16_t gameObjectCount;
    uint16_t textureAtlasCount;
    uint16_t clutCount;
    uint16_t pad;
};

struct SPLASHPACKTextureAtlas {
    uint32_t polygonsOffset;
    uint16_t width, height;
    uint16_t x, y;
};

struct SPLASHPACKClut {
    uint16_t clut[256];

    uint16_t clutPackingX;
    uint16_t clutPackingY;
    uint16_t length;
    uint16_t pad;
};

eastl::vector<GameObject *> LoadSplashpack(uint8_t *data);

};  // namespace psxsplash