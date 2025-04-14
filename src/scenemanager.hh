#pragma once

#include <EASTL/vector.h>

#include <psyqo/trigonometry.hh>
#include <psyqo/vector.hh>

#include "camera.hh"
#include "controls.hh"
#include "gameobject.hh"
#include "lua.h"
#include "splashpack.hh"

namespace psxsplash {
class SceneManager {
  public:
    void InitializeScene(uint8_t* splashpackData);
    void GameTick();

  private:
    psxsplash::Lua L;
    psxsplash::SplashPackLoader m_loader;

    eastl::vector<LuaFile*> m_luaFiles;
    eastl::vector<GameObject*> m_gameObjects;
    eastl::vector<Navmesh*> m_navmeshes;

    psxsplash::Controls m_controls;

    psxsplash::Camera m_currentCamera;

    psyqo::Vec3 m_playerPosition;
    psyqo::Angle playerRotationX, playerRotationY, playerRotationZ;

    psyqo::FixedPoint<12, uint16_t> m_playerHeight;

    bool previewNavmesh = false;
    bool freecam = false;
};
};  // namespace psxsplash