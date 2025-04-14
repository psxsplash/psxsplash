#include "scenemanager.hh"

#include <utility>

#include "navmesh.hh"
#include "renderer.hh"
#include "splashpack.hh"

using namespace psyqo::trig_literals;

void psxsplash::SceneManager::InitializeScene(uint8_t* splashpackData) {
    L.Init();

    SplashpackSceneSetup sceneSetup;
    m_loader.LoadSplashpack(splashpackData, sceneSetup);

    m_luaFiles = std::move(sceneSetup.luaFiles);
    m_gameObjects = std::move(sceneSetup.objects);
    m_navmeshes = std::move(sceneSetup.navmeshes);

    m_playerPosition = sceneSetup.playerStartPosition;

    playerRotationX = 0.0_pi;
    playerRotationY = 0.0_pi;
    playerRotationZ = 0.0_pi;

    m_playerHeight = sceneSetup.playerHeight;

    // Load Lua files
    for (int i = 0; i < m_luaFiles.size(); i++) {
        auto luaFile = m_luaFiles[i];
        L.LoadLuaFile(luaFile->luaCode, luaFile->length, i);
    }

    // Register game objects
    for (auto object : m_gameObjects) {
        L.RegisterGameObject(object);
    }

    m_controls.Init();
    Renderer::GetInstance().SetCamera(m_currentCamera);
}

void psxsplash::SceneManager::GameTick() {
    auto& renderer = psxsplash::Renderer::GetInstance();

    renderer.Render(m_gameObjects);

    m_controls.HandleControls(m_playerPosition, playerRotationX, playerRotationY, playerRotationZ, false, 1);
    if (!freecam) {
        psxsplash::ComputeNavmeshPosition(m_playerPosition, *m_navmeshes[0],
                                          static_cast<psyqo::FixedPoint<12>>(m_playerHeight));
    }

    m_currentCamera.SetPosition(static_cast<psyqo::FixedPoint<12>>(m_playerPosition.x),
                                static_cast<psyqo::FixedPoint<12>>(m_playerPosition.y),
                                static_cast<psyqo::FixedPoint<12>>(m_playerPosition.z));
    m_currentCamera.SetRotation(playerRotationX, playerRotationY, playerRotationZ);
}