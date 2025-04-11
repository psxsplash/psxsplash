#pragma once

#include <EASTL/vector.h>

#include <cstdint>

#include "gameobject.hh"
#include "lua.h"
#include "navmesh.hh"
#include "psyqo/fixed-point.hh"

namespace psxsplash {

  
class SplashPackLoader {
  public:
    eastl::vector<GameObject *> gameObjects;
    eastl::vector<Navmesh *> navmeshes;
    
    psyqo::GTE::PackedVec3 playerStartPos, playerStartRot;
    psyqo::FixedPoint<12, uint16_t> playerHeight;

    void LoadSplashpack(uint8_t *data, Lua &lua);
};

};  // namespace psxsplash