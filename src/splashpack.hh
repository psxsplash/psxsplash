#pragma once

#include <EASTL/vector.h>

#include <cstdint>

#include "gameobject.hh"
#include "navmesh.hh"

namespace psxsplash {

class SplashPackLoader {
  public:
    eastl::vector<GameObject *> gameObjects;
    eastl::vector<Navmesh *> navmeshes;
    void LoadSplashpack(uint8_t *data);
};

};  // namespace psxsplash