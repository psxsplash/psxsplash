#pragma once

#include <EASTL/vector.h>

#include <psyqo/fixed-point.hh>

#include "gameobject.hh"
#include "lua.h"
#include "navmesh.hh"

namespace psxsplash {

struct SplashpackSceneSetup {
    LuaFile* sceneLuaFile;
    eastl::vector<LuaFile *> luaFiles;
    eastl::vector<GameObject *> objects;
    eastl::vector<Navmesh *> navmeshes;
    psyqo::GTE::PackedVec3 playerStartPosition;
    psyqo::GTE::PackedVec3 playerStartRotation;
    psyqo::FixedPoint<12, uint16_t> playerHeight;
};

class SplashPackLoader {
  public:
    void LoadSplashpack(uint8_t *data, SplashpackSceneSetup &setup);
};

};  // namespace psxsplash