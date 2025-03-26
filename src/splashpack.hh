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
};

struct SPLASHPACKTextureAtlas {
    uint32_t polygonsOffset;
    uint16_t width, height;
    uint16_t x, y;
};

eastl::vector<GameObject *> LoadSplashpack(uint8_t *data);

};  // namespace psxsplash