#include "scenemanager.hh"

#include <utility>

#include "collision.hh"
#include "profiler.hh"
#include "renderer.hh"
#include "splashpack.hh"
#include "streq.hh"
#include "luaapi.hh"
#include "loadingscreen.hh"

#include <psyqo/primitives/misc.hh>
#include <psyqo/trigonometry.hh>

#if defined(LOADER_CDROM)
#include "cdromhelper.hh"
#endif

#include "lua.h"

using namespace psyqo::trig_literals;

using namespace psyqo::fixed_point_literals;

using namespace psxsplash;

// Static member definition
psyqo::Font<>* psxsplash::SceneManager::s_font = nullptr;

// Default player collision radius: ~0.5 world units at GTE 100 -> 20 in 20.12
static constexpr int32_t PLAYER_RADIUS = 20;

// Interaction system state
static psyqo::Trig<> s_interactTrig;
static int s_activePromptCanvas = -1;  // Currently shown prompt canvas index (-1 = none)

void psxsplash::SceneManager::InitializeScene(uint8_t* splashpackData, LoadingScreen* loading) {
    auto& gpu = Renderer::GetInstance().getGPU();

    L.Reset();
    
    // Initialize audio system
    m_audio.init();
    
    // Register the Lua API
    LuaAPI::RegisterAll(L.getState(), this, &m_cutscenePlayer, &m_animationPlayer, &m_uiSystem);

#ifdef PSXSPLASH_PROFILER
    debug::Profiler::getInstance().initialize();
#endif

    SplashpackSceneSetup sceneSetup;
    m_loader.LoadSplashpack(splashpackData, sceneSetup);

    if (loading && loading->isActive()) loading->updateProgress(gpu, 40);

    m_luaFiles = std::move(sceneSetup.luaFiles);
    m_gameObjects = std::move(sceneSetup.objects);
    m_objectNames = std::move(sceneSetup.objectNames);
    m_bvh = sceneSetup.bvh;  // Copy BVH for frustum culling
    m_navRegions = sceneSetup.navRegions;          // Nav region system (v7+)
    m_playerNavRegion = m_navRegions.isLoaded() ? m_navRegions.getStartRegion() : NAV_NO_REGION;

    // If nav regions are loaded, camera follows the player. Otherwise the
    // scene is in "free camera" mode where cutscenes and Lua drive the camera.
    m_cameraFollowsPlayer = m_navRegions.isLoaded();
    m_controlsEnabled = true;

    // Scene type and render path
    m_sceneType = sceneSetup.sceneType;

    // Room/portal data for interior scenes (v11+)
    m_rooms = sceneSetup.rooms;
    m_roomCount = sceneSetup.roomCount;
    m_portals = sceneSetup.portals;
    m_portalCount = sceneSetup.portalCount;
    m_roomTriRefs = sceneSetup.roomTriRefs;
    m_roomTriRefCount = sceneSetup.roomTriRefCount;
    m_roomCells = sceneSetup.roomCells;
    m_roomCellCount = sceneSetup.roomCellCount;
    m_roomPortalRefs = sceneSetup.roomPortalRefs;
    m_roomPortalRefCount = sceneSetup.roomPortalRefCount;

    // Configure fog and back color from splashpack data (v11+)
    {
        psxsplash::FogConfig fogCfg;
        fogCfg.enabled = sceneSetup.fogEnabled;
        fogCfg.color = {.r = sceneSetup.fogR, .g = sceneSetup.fogG, .b = sceneSetup.fogB};
        fogCfg.density = sceneSetup.fogDensity;
        Renderer::GetInstance().SetFog(fogCfg);
    }    
    // Copy component arrays
    m_interactables = std::move(sceneSetup.interactables);

    // Load audio clips into SPU RAM
    m_audioClipNames = std::move(sceneSetup.audioClipNames);
    for (size_t i = 0; i < sceneSetup.audioClips.size(); i++) {
        auto& clip = sceneSetup.audioClips[i];
        m_audio.loadClip((int)i, clip.adpcmData, clip.sizeBytes, clip.sampleRate, clip.loop);
    }

    if (loading && loading->isActive()) loading->updateProgress(gpu, 55);

    // Copy cutscene data into scene manager storage (sceneSetup is stack-local)
    m_cutsceneCount = sceneSetup.cutsceneCount;
    for (int i = 0; i < m_cutsceneCount; i++) {
        m_cutscenes[i] = sceneSetup.loadedCutscenes[i];
    }

    // Initialize cutscene player (v12+)
    m_cutscenePlayer.init(
        m_cutsceneCount > 0 ? m_cutscenes : nullptr,
        m_cutsceneCount,
        &m_currentCamera,
        &m_audio,
        &m_uiSystem,
        this
    );

    // Copy animation data into scene manager storage
    m_animationCount = sceneSetup.animationCount;
    for (int i = 0; i < m_animationCount; i++) {
        m_animations[i] = sceneSetup.loadedAnimations[i];
    }

    // Initialize animation player
    m_animationPlayer.init(
        m_animationCount > 0 ? m_animations : nullptr,
        m_animationCount,
        &m_uiSystem,
        this
    );

    // Copy skinned mesh data from splashpack into scene manager storage
    m_skinnedMeshCount = sceneSetup.skinnedMeshCount;
    for (int i = 0; i < m_skinnedMeshCount; i++) {
        m_skinAnimSets[i] = sceneSetup.loadedSkinAnimSets[i];
        m_skinAnimStates[i] = SkinAnimState{};
        m_skinAnimStates[i].animSet = &m_skinAnimSets[i];

        uint16_t goIdx = m_skinAnimSets[i].gameObjectIndex;
        if (goIdx < m_gameObjects.size()) {
            GameObject* go = m_gameObjects[goIdx];
            m_skinAnimSets[i].polygons  = go->polygons;
            m_skinAnimSets[i].polyCount = go->polyCount;
            go->polyCount = 0;
            go->flagsAsInt |= 0x10;
        } else {
            m_skinAnimSets[i].polygons  = nullptr;
            m_skinAnimSets[i].polyCount = 0;
        }
    }
    Renderer::GetInstance().SetSkinData(
        m_skinnedMeshCount > 0 ? m_skinAnimSets : nullptr,
        m_skinnedMeshCount > 0 ? m_skinAnimStates : nullptr,
        m_skinnedMeshCount);

    // Initialize UI system (v13+)
    if (sceneSetup.uiCanvasCount > 0 && sceneSetup.uiTableOffset != 0 && s_font != nullptr) {
        m_uiSystem.init(*s_font);
        m_uiSystem.loadFromSplashpack(splashpackData, sceneSetup.uiCanvasCount,
                                      sceneSetup.uiFontCount, sceneSetup.uiTableOffset);
        m_uiSystem.uploadFonts(Renderer::GetInstance().getGPU());
        Renderer::GetInstance().SetUISystem(&m_uiSystem);

        if (loading && loading->isActive()) loading->updateProgress(gpu, 70);

        // Resolve UI track handles: the splashpack loader stored raw name pointers
        // in CutsceneTrack.target for UI tracks. Now that UISystem is loaded, resolve
        // those names to canvas indices / element handles.
        for (int ci = 0; ci < m_cutsceneCount; ci++) {
            for (uint8_t ti = 0; ti < m_cutscenes[ci].trackCount; ti++) {
                auto& track = m_cutscenes[ci].tracks[ti];
                bool isUI = isUITrackType(track.trackType);
                if (!isUI || track.target == nullptr) continue;

                const char* nameStr = reinterpret_cast<const char*>(track.target);
                track.target = nullptr; // Clear the temporary name pointer

                if (track.trackType == TrackType::UICanvasVisible) {
                    // Name is just the canvas name
                    track.uiHandle = static_cast<int16_t>(m_uiSystem.findCanvas(nameStr));
                } else {
                    // Name is "canvasName/elementName" — find the '/' separator
                    const char* sep = nameStr;
                    while (*sep && *sep != '/') sep++;
                    if (*sep == '/') {
                        // Temporarily null-terminate the canvas portion
                        // (nameStr points into splashpack data, which is mutable)
                        char* mutableSep = const_cast<char*>(sep);
                        *mutableSep = '\0';
                        int canvasIdx = m_uiSystem.findCanvas(nameStr);
                        *mutableSep = '/'; // Restore the separator
                        if (canvasIdx >= 0) {
                            track.uiHandle = static_cast<int16_t>(
                                m_uiSystem.findElement(canvasIdx, sep + 1));
                        }
                    }
                }
            }
        }

        // Resolve UI track handles for animation tracks (same logic)
        for (int ai = 0; ai < m_animationCount; ai++) {
            for (uint8_t ti = 0; ti < m_animations[ai].trackCount; ti++) {
                auto& track = m_animations[ai].tracks[ti];
                bool isUI = isUITrackType(track.trackType);
                if (!isUI || track.target == nullptr) continue;

                const char* nameStr = reinterpret_cast<const char*>(track.target);
                track.target = nullptr;

                if (track.trackType == TrackType::UICanvasVisible) {
                    track.uiHandle = static_cast<int16_t>(m_uiSystem.findCanvas(nameStr));
                } else {
                    const char* sep = nameStr;
                    while (*sep && *sep != '/') sep++;
                    if (*sep == '/') {
                        char* mutableSep = const_cast<char*>(sep);
                        *mutableSep = '\0';
                        int canvasIdx = m_uiSystem.findCanvas(nameStr);
                        *mutableSep = '/';
                        if (canvasIdx >= 0) {
                            track.uiHandle = static_cast<int16_t>(
                                m_uiSystem.findElement(canvasIdx, sep + 1));
                        }
                    }
                }
            }
        }
    } else {
        Renderer::GetInstance().SetUISystem(nullptr);
    }

#ifdef PSXSPLASH_MEMOVERLAY
    if (s_font != nullptr) {
        m_memOverlay.init(s_font);
        Renderer::GetInstance().SetMemOverlay(&m_memOverlay);
    }
#endif

    m_playerPosition = sceneSetup.playerStartPosition;

    playerRotationX = 0.0_pi;
    playerRotationY = 0.0_pi;
    playerRotationZ = 0.0_pi;

    m_playerHeight = sceneSetup.playerHeight;

    m_controls.setMoveSpeed(sceneSetup.moveSpeed);
    m_controls.setSprintSpeed(sceneSetup.sprintSpeed);
    m_playerRadius = (int32_t)sceneSetup.playerRadius.value;
    if (m_playerRadius == 0) m_playerRadius = PLAYER_RADIUS; 
    m_jumpVelocityRaw = (int32_t)sceneSetup.jumpVelocity.value;
    int32_t gravityRaw = (int32_t)sceneSetup.gravity.value;
    m_gravityPerFrame = gravityRaw / 30;  
    if (m_gravityPerFrame == 0 && gravityRaw > 0) m_gravityPerFrame = 1; 
    m_velocityY = 0;
    m_isGrounded = true;
    m_lastFrameTime = 0;
    m_dt12 = 4096;  // Default: 1.0 frame

    m_collisionSystem.init();
    
    for (size_t i = 0; i < sceneSetup.colliders.size(); i++) {
        SPLASHPACKCollider* collider = sceneSetup.colliders[i];
        if (collider == nullptr) continue;
        
        AABB bounds;
        bounds.min.x.value = collider->minX;
        bounds.min.y.value = collider->minY;
        bounds.min.z.value = collider->minZ;
        bounds.max.x.value = collider->maxX;
        bounds.max.y.value = collider->maxY;
        bounds.max.z.value = collider->maxZ;
        
        CollisionType type = static_cast<CollisionType>(collider->collisionType);
        
        m_collisionSystem.registerCollider(
            collider->gameObjectIndex,
            bounds,
            type,
            collider->layerMask
        );
    }

    for (size_t i = 0; i < sceneSetup.triggerBoxes.size(); i++) {
        SPLASHPACKTriggerBox* tb = sceneSetup.triggerBoxes[i];
        if (tb == nullptr) continue;

        AABB bounds;
        bounds.min.x.value = tb->minX;
        bounds.min.y.value = tb->minY;
        bounds.min.z.value = tb->minZ;
        bounds.max.x.value = tb->maxX;
        bounds.max.y.value = tb->maxY;
        bounds.max.z.value = tb->maxZ;

        m_collisionSystem.registerTriggerBox(bounds, tb->luaFileIndex);
    }


    for (int i = 0; i < m_luaFiles.size(); i++) {
        auto luaFile = m_luaFiles[i];
        L.LoadLuaFile(luaFile->luaCode, luaFile->length, i);
    }

    if (loading && loading->isActive()) loading->updateProgress(gpu, 85);

    L.RegisterSceneScripts(sceneSetup.sceneLuaFileIndex);

    L.OnSceneCreationStart();

    for (auto object : m_gameObjects) {
        L.RegisterGameObject(object);
    }

    // Fire all onCreate events AFTER all objects are registered,
    // so Entity.Find works across all objects in onCreate handlers.
    if (!m_gameObjects.empty()) {
        L.FireAllOnCreate(
            reinterpret_cast<GameObject**>(m_gameObjects.data()),
            m_gameObjects.size());
    }

    m_controls.forceAnalogMode();
    m_controls.Init();
    Renderer::GetInstance().SetCamera(m_currentCamera);

    L.OnSceneCreationEnd();

    if (loading && loading->isActive()) loading->updateProgress(gpu, 95);

    m_liveDataSize = sceneSetup.liveDataSize;
    shrinkBuffer();

    if (loading && loading->isActive()) loading->updateProgress(gpu, 100);
}

void psxsplash::SceneManager::GameTick(psyqo::GPU &gpu) {
    LuaAPI::IncrementFrameCount();
    
    {
        uint32_t now = gpu.now();
        if (m_lastFrameTime != 0) {
            uint32_t elapsed = now - m_lastFrameTime;

            if (elapsed > 200000) elapsed = 200000;  // cap at ~6 frames
            m_dt12 = (int32_t)((elapsed * 4096u) / 33333u);
            if (m_dt12 < 1) m_dt12 = 1;              // minimum: tiny fraction
            if (m_dt12 > 4096 * 4) m_dt12 = 4096 * 4; // cap at 4 frames
        }
        m_lastFrameTime = now;
    }

    m_cutscenePlayer.tick(m_dt12);
    m_animationPlayer.tick(m_dt12);

    // Tick skinned mesh animations
    for (int i = 0; i < m_skinnedMeshCount; i++) {
        SkinMesh_Tick(&m_skinAnimStates[i], L.getState().getState(), m_dt12);
    }
    
    uint32_t renderingStart = gpu.now();
    auto& renderer = psxsplash::Renderer::GetInstance();

    if (m_roomCount > 0 && m_rooms != nullptr) {

        int camRoom = -1;
        if (m_navRegions.isLoaded()) {
            if (m_cutscenePlayer.isPlaying() && m_cutscenePlayer.hasCameraTracks()) {
                auto& camPos = m_currentCamera.GetPosition();
                uint16_t camRegion = m_navRegions.findRegion(camPos.x.value, camPos.z.value);
                if (camRegion != NAV_NO_REGION) {
                    uint8_t ri = m_navRegions.getRoomIndex(camRegion);
                    if (ri != 0xFF) camRoom = (int)ri;
                }
            } else if (m_playerNavRegion != NAV_NO_REGION) {
                uint8_t ri = m_navRegions.getRoomIndex(m_playerNavRegion);
                if (ri != 0xFF) camRoom = (int)ri;
            }
        }
        renderer.RenderWithRooms(m_gameObjects, m_rooms, m_roomCount,
                                  m_portals, m_portalCount, m_roomTriRefs,
                                  m_roomCells, m_roomPortalRefs, camRoom);
    } else {
        renderer.RenderWithBVH(m_gameObjects, m_bvh);
    }
    gpu.pumpCallbacks();
    uint32_t renderingEnd = gpu.now();
    uint32_t renderingTime = renderingEnd - renderingStart;

#ifdef PSXSPLASH_PROFILER
    psxsplash::debug::Profiler::getInstance().setSectionTime(psxsplash::debug::PROFILER_RENDERING, renderingTime);
#endif

    uint32_t collisionStart = gpu.now();
    
    AABB playerAABB;
    {
        psyqo::FixedPoint<12> r;
        r.value = m_playerRadius;
        psyqo::FixedPoint<12> px = static_cast<psyqo::FixedPoint<12>>(m_playerPosition.x);
        psyqo::FixedPoint<12> py = static_cast<psyqo::FixedPoint<12>>(m_playerPosition.y);
        psyqo::FixedPoint<12> pz = static_cast<psyqo::FixedPoint<12>>(m_playerPosition.z);
        psyqo::FixedPoint<12> h = static_cast<psyqo::FixedPoint<12>>(m_playerHeight);
        // Y is inverted on PS1: negative = up, positive = down.
        // m_playerPosition.y is the camera (head), feet are at py + h.
        // Leave a small gap at the bottom so the floor geometry doesn't
        // trigger constant collisions (floor contact is handled by nav).
        psyqo::FixedPoint<12> bodyBottom;
        bodyBottom.value = h.value * 3 / 4;  // 75% of height below camera
        playerAABB.min = psyqo::Vec3{px - r, py, pz - r};
        playerAABB.max = psyqo::Vec3{px + r, py + bodyBottom, pz + r};
    }
    
    psyqo::Vec3 pushBack;
    int collisionCount = m_collisionSystem.detectCollisions(playerAABB, pushBack, *this);
    
    {
        psyqo::FixedPoint<12> zero;
        if (pushBack.x != zero || pushBack.z != zero) {
            m_playerPosition.x = m_playerPosition.x + pushBack.x;
            m_playerPosition.z = m_playerPosition.z + pushBack.z;
        }
    }
    
    // Fire onCollideWithPlayer Lua events on collided objects
    const CollisionResult* results = m_collisionSystem.getResults();
    for (int i = 0; i < collisionCount; i++) {
        if (results[i].objectA != 0xFFFF) continue;
        auto* obj = getGameObject(results[i].objectB);
        if (obj) {
            L.OnCollideWithPlayer(obj);
        }
    }
    
    // Process trigger boxes (enter/exit)
    m_collisionSystem.detectTriggers(playerAABB, *this);
    
    gpu.pumpCallbacks();
    uint32_t collisionEnd = gpu.now();
    
    uint32_t luaStart = gpu.now();
    // Lua update tick - call onUpdate for all registered objects with onUpdate handler
    for (auto* go : m_gameObjects) {
        if (go && go->isActive()) {
            L.OnUpdate(go, m_dt12);
        }
    }
    gpu.pumpCallbacks();
    uint32_t luaEnd = gpu.now();
    uint32_t luaTime = luaEnd - luaStart;
#ifdef PSXSPLASH_PROFILER
    psxsplash::debug::Profiler::getInstance().setSectionTime(psxsplash::debug::PROFILER_LUA, luaTime);
#endif
    
    // Update game systems
    processEnableDisableEvents();

    
    uint32_t controlsStart = gpu.now();
    
    // Update button state tracking first
    m_controls.UpdateButtonStates();
    
    // Update interaction system (checks for interact button press)
    updateInteractionSystem();
    
    // Dispatch button events to all objects
    uint16_t pressed = m_controls.getButtonsPressed();
    uint16_t released = m_controls.getButtonsReleased();
    
    if (pressed || released) {
        // Only iterate objects if there are button events
        for (auto* go : m_gameObjects) {
            if (!go || !go->isActive()) continue;
            
            if (pressed) {
                // Dispatch press events for each pressed button
                for (int btn = 0; btn < 16; btn++) {
                    if (pressed & (1 << btn)) {
                        L.OnButtonPress(go, btn);
                    }
                }
            }
            if (released) {
                // Dispatch release events for each released button
                for (int btn = 0; btn < 16; btn++) {
                    if (released & (1 << btn)) {
                        L.OnButtonRelease(go, btn);
                    }
                }
            }
        }
    }
    
    // Save position BEFORE movement for collision detection
    psyqo::Vec3 oldPlayerPosition = m_playerPosition;

    if (m_controlsEnabled) {
        m_controls.HandleControls(m_playerPosition, playerRotationX, playerRotationY, playerRotationZ, freecam, m_dt12);

        // Jump input: Cross button triggers jump when grounded
        if (m_isGrounded && m_controls.wasButtonPressed(psyqo::AdvancedPad::Button::Cross)) {
            m_velocityY = -m_jumpVelocityRaw;  // Negative = upward (PSX Y-down)
            m_isGrounded = false;
        }
    }
    
    gpu.pumpCallbacks();
    uint32_t controlsEnd = gpu.now();
    uint32_t controlsTime = controlsEnd - controlsStart;
#ifdef PSXSPLASH_PROFILER
    psxsplash::debug::Profiler::getInstance().setSectionTime(psxsplash::debug::PROFILER_CONTROLS, controlsTime);
#endif

    uint32_t navmeshStart = gpu.now();
    if (!freecam && m_navRegions.isLoaded()) {
        // Apply gravity scaled by dt12 (4096 = 1 frame)
        int32_t gravityDelta = (int32_t)(((int64_t)m_gravityPerFrame * m_dt12) >> 12);
        m_velocityY += gravityDelta;

        // Apply vertical velocity to position, scaled by dt12
        int32_t posYDelta = (int32_t)(((int64_t)m_velocityY * m_dt12) >> 12);
        m_playerPosition.y.value += posYDelta;

        // Resolve position via nav regions
        uint16_t prevRegion = m_playerNavRegion;
        int32_t px = m_playerPosition.x.value;
        int32_t py = m_playerPosition.y.value;
        int32_t pz = m_playerPosition.z.value;
        int32_t floorY = m_navRegions.resolvePosition(
            px, py, pz, m_playerNavRegion);

        if (m_playerNavRegion != NAV_NO_REGION) {
            m_playerPosition.x.value = px;
            m_playerPosition.z.value = pz;

            int32_t cameraAtFloor = floorY - m_playerHeight.raw();

            if (m_playerPosition.y.value >= cameraAtFloor) {
                m_playerPosition.y.value = cameraAtFloor;
                m_velocityY = 0;
                m_isGrounded = true;
            } else {
                m_isGrounded = false;
            }
        } else {
            m_playerPosition = oldPlayerPosition;
            m_playerNavRegion = prevRegion;
            m_velocityY = 0;
            m_isGrounded = true;
        }
    }



    gpu.pumpCallbacks();
    uint32_t navmeshEnd = gpu.now();
    uint32_t navmeshTime = navmeshEnd - navmeshStart;
#ifdef PSXSPLASH_PROFILER
    psxsplash::debug::Profiler::getInstance().setSectionTime(psxsplash::debug::PROFILER_NAVMESH, navmeshTime);
#endif

    // Only snap camera to player when in player-follow mode and no
    // cutscene is actively controlling the camera. In free camera mode
    // (no nav regions / no PSXPlayer), the camera is driven entirely
    // by cutscenes and Lua. After a cutscene ends in free mode, the
    // camera stays at the last cutscene position.
    if (m_cameraFollowsPlayer && !(m_cutscenePlayer.isPlaying() && m_cutscenePlayer.hasCameraTracks())) {
        m_currentCamera.SetPosition(static_cast<psyqo::FixedPoint<12>>(m_playerPosition.x),
                                    static_cast<psyqo::FixedPoint<12>>(m_playerPosition.y),
                                    static_cast<psyqo::FixedPoint<12>>(m_playerPosition.z));
        m_currentCamera.SetRotation(playerRotationX, playerRotationY, playerRotationZ);
    }

    // Process pending scene transitions (at end of frame)
    processPendingSceneLoad();
}

void psxsplash::SceneManager::fireTriggerEnter(int16_t luaFileIndex, uint16_t triggerIndex) {
    if (luaFileIndex < 0) return;
    L.OnTriggerEnterScript(luaFileIndex, triggerIndex);
}

void psxsplash::SceneManager::fireTriggerExit(int16_t luaFileIndex, uint16_t triggerIndex) {
    if (luaFileIndex < 0) return;
    L.OnTriggerExitScript(luaFileIndex, triggerIndex);
}

// ============================================================================
// INTERACTION SYSTEM
// ============================================================================

void psxsplash::SceneManager::updateInteractionSystem() {
    // Tick cooldowns for all interactables
    for (auto* interactable : m_interactables) {
        if (interactable) interactable->update();
    }

    // Player position for distance checks
    psyqo::FixedPoint<12> playerX = static_cast<psyqo::FixedPoint<12>>(m_playerPosition.x);
    psyqo::FixedPoint<12> playerY = static_cast<psyqo::FixedPoint<12>>(m_playerPosition.y);
    psyqo::FixedPoint<12> playerZ = static_cast<psyqo::FixedPoint<12>>(m_playerPosition.z);

    // Player forward direction from Y rotation (for line-of-sight checks)
    psyqo::FixedPoint<12> forwardX = s_interactTrig.sin(playerRotationY);
    psyqo::FixedPoint<12> forwardZ = s_interactTrig.cos(playerRotationY);

    // First pass: find which interactable is closest and in range (for prompt display)
    Interactable* inRange = nullptr;
    psyqo::FixedPoint<12> closestDistSq;
    closestDistSq.value = 0x7FFFFFFF;

    for (auto* interactable : m_interactables) {
        if (!interactable) continue;
        if (interactable->isDisabled()) continue;

        auto* go = getGameObject(interactable->gameObjectIndex);
        if (!go || !go->isActive()) continue;

        // Distance check
        psyqo::FixedPoint<12> dx = playerX - go->position.x;
        psyqo::FixedPoint<12> dy = playerY - go->position.y;
        psyqo::FixedPoint<12> dz = playerZ - go->position.z;
        psyqo::FixedPoint<12> distSq = dx * dx + dy * dy + dz * dz;

        if (distSq > interactable->radiusSquared) continue;

        // Line-of-sight check: dot product of forward vector and direction to object
        if (interactable->requireLineOfSight()) {
            // dot = forwardX * dx + forwardZ * dz (XZ plane only)
            // Negative dot means object is behind the player
            psyqo::FixedPoint<12> dot = forwardX * dx + forwardZ * dz;
            // Object must be in front of the player (dot < 0 in the coordinate system
            // because dx points FROM player TO object, and forward points where player faces)
            // Actually: dx = playerX - objX, so it points FROM object TO player.
            // We want the object in front, so we need -dx direction to align with forward.
            // dot(forward, objDir) where objDir = obj - player = -dx, -dz
            psyqo::FixedPoint<12> facingDot = -(forwardX * dx + forwardZ * dz);
            if (facingDot.value <= 0) continue;  // Object is behind the player
        }

        if (distSq < closestDistSq) {
            inRange = interactable;
            closestDistSq = distSq;
        }
    }

    // Prompt canvas management: show only when in range AND can interact
    int newPromptCanvas = -1;
    if (inRange && inRange->canInteract() && inRange->showPrompt() && inRange->promptCanvasName[0] != '\0') {
        newPromptCanvas = m_uiSystem.findCanvas(inRange->promptCanvasName);
    }

    if (newPromptCanvas != s_activePromptCanvas) {
        // Hide old prompt
        if (s_activePromptCanvas >= 0) {
            m_uiSystem.setCanvasVisible(s_activePromptCanvas, false);
        }
        // Show new prompt
        if (newPromptCanvas >= 0) {
            m_uiSystem.setCanvasVisible(newPromptCanvas, true);
        }
        s_activePromptCanvas = newPromptCanvas;
    }

    // Check if the closest in-range interactable can be activated
    if (!inRange || !inRange->canInteract()) return;

    // Check if the correct button for this interactable was pressed
    auto button = static_cast<psyqo::AdvancedPad::Button>(
        static_cast<uint16_t>(inRange->interactButton));
    if (!m_controls.wasButtonPressed(button)) return;

    // Trigger the interaction
    triggerInteraction(getGameObject(inRange->gameObjectIndex));
    inRange->triggerCooldown();
}

void psxsplash::SceneManager::triggerInteraction(GameObject* interactable) {
    if (!interactable) return;
    L.OnInteract(interactable);
}

// ============================================================================
// ENABLE/DISABLE SYSTEM
// ============================================================================

void psxsplash::SceneManager::setObjectActive(GameObject* go, bool active) {
    if (!go) return;
    
    bool wasActive = go->isActive();
    if (wasActive == active) return;  // No change
    
    go->setActive(active);
    
    // Fire appropriate event
    if (active) {
        L.OnEnable(go);
    } else {
        L.OnDisable(go);
    }
}

void psxsplash::SceneManager::processEnableDisableEvents() {
    // Process any pending enable/disable flags.
    // Uses raw bit manipulation on flagsAsInt instead of the BitField
    // accessors to avoid a known issue where the BitSpan get/set
    // operations don't behave correctly on the MIPS target.
    for (auto* go : m_gameObjects) {
        if (!go) continue;

        // Bit 1 = pendingEnable
        if (go->flagsAsInt & 0x02) {
            go->flagsAsInt &= ~0x02u;  // clear pending
            if (!(go->flagsAsInt & 0x01)) {  // if not already active
                go->flagsAsInt |= 0x01;  // set active
                L.OnEnable(go);
            }
        }

        // Bit 2 = pendingDisable
        if (go->flagsAsInt & 0x04) {
            go->flagsAsInt &= ~0x04u;  // clear pending
            if (go->flagsAsInt & 0x01) {  // if currently active
                go->flagsAsInt &= ~0x01u;  // clear active
                L.OnDisable(go);
            }
        }
    }
}

// ============================================================================
// PLAYER
// ============================================================================

psyqo::Vec3& psxsplash::SceneManager::getPlayerPosition(){
    return m_playerPosition;
}

void psxsplash::SceneManager::setPlayerPosition(psyqo::FixedPoint<12> x, psyqo::FixedPoint<12> y, psyqo::FixedPoint<12> z){
    m_playerPosition.x = x;
    m_playerPosition.y = y;
    m_playerPosition.z = z;
}

psyqo::Vec3 psxsplash::SceneManager::getPlayerRotation(){
    psyqo::Vec3 playerRot;

    playerRot.x = (psyqo::FixedPoint<12>)playerRotationX;
    playerRot.y = (psyqo::FixedPoint<12>)playerRotationY;
    playerRot.z = (psyqo::FixedPoint<12>)playerRotationZ;

    return playerRot;
}

void psxsplash::SceneManager::setPlayerRotation(psyqo::FixedPoint<12> x, psyqo::FixedPoint<12> y, psyqo::FixedPoint<12> z){
   playerRotationX = (psyqo::FixedPoint<10>)x;
   playerRotationY = (psyqo::FixedPoint<10>)y;
   playerRotationZ = (psyqo::FixedPoint<10>)z;
}

// ============================================================================
// SCENE LOADING
// ============================================================================

void psxsplash::SceneManager::requestSceneLoad(int sceneIndex) {
    m_pendingSceneIndex = sceneIndex;
}

void psxsplash::SceneManager::processPendingSceneLoad() {
    if (m_pendingSceneIndex < 0) return;

    int targetIndex = m_pendingSceneIndex;
    m_pendingSceneIndex = -1;

    auto& gpu = Renderer::GetInstance().getGPU();
    loadScene(gpu, targetIndex, /*isFirstScene=*/false);
}

void psxsplash::SceneManager::loadScene(psyqo::GPU& gpu, int sceneIndex, bool isFirstScene) {
    // Restore CD-ROM controller and CPU IRQ state for file loading.
#if defined(LOADER_CDROM)
    CDRomHelper::WakeDrive();
#endif

    // Build filename using the active backend's naming convention
    char filename[32];
    FileLoader::BuildSceneFilename(sceneIndex, filename, sizeof(filename));

    psyqo::Prim::FastFill ff(psyqo::Color{.r = 0, .g = 0, .b = 0});
    ff.rect = psyqo::Rect{0, 0, 320, 240};
    gpu.sendPrimitive(ff);
    ff.rect = psyqo::Rect{0, 256, 320, 240};
    gpu.sendPrimitive(ff);
    gpu.pumpCallbacks();

    LoadingScreen loading;
    if (s_font) {
        if (loading.load(gpu, *s_font, sceneIndex)) {
            loading.renderInitialAndFree(gpu);
        }
    }

    if (!isFirstScene) {
        // Tear down EVERYTHING in the current scene first —
        // Lua VM, vector backing storage, audio.  This returns as much
        // heap memory as possible before any new allocation.
        clearScene();

        // Free old splashpack data BEFORE loading the new one.
        // This avoids having both scene buffers in the heap simultaneously.
        if (m_currentSceneData) {
            FileLoader::Get().FreeFile(m_currentSceneData);
            m_currentSceneData = nullptr;
        }
    }

    if (loading.isActive()) loading.updateProgress(gpu, 20);

    // Load scene data — use progress-aware variant so the loading bar
    // animates during the (potentially slow) CD-ROM read.
    int fileSize = 0;
    uint8_t* newData = nullptr;

    if (loading.isActive()) {
        struct Ctx { LoadingScreen* ls; psyqo::GPU* gpu; };
        Ctx ctx{&loading, &gpu};
        FileLoader::LoadProgressInfo progress{
            [](uint8_t pct, void* ud) {
                auto* c = static_cast<Ctx*>(ud);
                c->ls->updateProgress(*c->gpu, pct);
            },
            &ctx, 20, 30
        };
        newData = FileLoader::Get().LoadFileSyncWithProgress(
            filename, fileSize, &progress);
    } else {
        newData = FileLoader::Get().LoadFileSync(filename, fileSize);
    }

    if (!newData && isFirstScene) {
        // Fallback: try legacy name for backwards compatibility (PCdrv only)
        newData = FileLoader::Get().LoadFileSync("output.bin", fileSize);
    }

    if (!newData) {
        return;
    }

    if (loading.isActive()) loading.updateProgress(gpu, 30);

    // Stop the CD-ROM motor and mask all interrupts for gameplay.
#if defined(LOADER_CDROM)
    CDRomHelper::SilenceDrive();
#endif

    m_currentSceneData = newData;
    m_currentSceneIndex = sceneIndex;

    // Initialize with new data (creates fresh Lua VM inside)
    InitializeScene(newData, loading.isActive() ? &loading : nullptr);
}

void psxsplash::SceneManager::clearScene() {
    // 1. Shut down the Lua VM first — frees ALL Lua-allocated memory
    //    (bytecode, strings, tables, registry) in one shot via lua_close.
    L.Shutdown();

    // 2. Clear all vectors to free their heap storage (game objects, Lua files, names, etc)
    { eastl::vector<GameObject*>    tmp; tmp.swap(m_gameObjects);    }
    { eastl::vector<LuaFile*>       tmp; tmp.swap(m_luaFiles);       }
    { eastl::vector<const char*>    tmp; tmp.swap(m_objectNames);    }
    { eastl::vector<const char*>    tmp; tmp.swap(m_audioClipNames); }
    { eastl::vector<Interactable*>  tmp; tmp.swap(m_interactables);  }

    // 3. Reset hardware / subsystems
    m_audio.reset();           // Free SPU RAM and stop all voices
    m_collisionSystem.init();  // Re-init collision system
    m_cutsceneCount = 0;
    s_activePromptCanvas = -1; // Reset prompt tracking
    m_cutscenePlayer.init(nullptr, 0, nullptr, nullptr);  // Reset cutscene player
    m_animationCount = 0;
    m_animationPlayer.init(nullptr, 0);  // Reset animation player
    m_skinnedMeshCount = 0;
    Renderer::GetInstance().SetSkinData(nullptr, nullptr, 0);
    // BVH and NavRegions will be overwritten by next load
    
    // Reset UI system (disconnect from renderer before splashpack data disappears)
    Renderer::GetInstance().SetUISystem(nullptr);

    // Reset room/portal pointers (they point into splashpack data which is being freed)
    m_rooms = nullptr;
    m_roomCount = 0;
    m_portals = nullptr;
    m_portalCount = 0;
    m_roomTriRefs = nullptr;
    m_roomTriRefCount = 0;
    m_roomCells = nullptr;
    m_roomCellCount = 0;
    m_roomPortalRefs = nullptr;
    m_roomPortalRefCount = 0;
    m_sceneType = 0;
}

void psxsplash::SceneManager::shrinkBuffer() {
    if (m_liveDataSize == 0 || m_currentSceneData == nullptr) return;

    uint8_t* oldBase = m_currentSceneData;

    uint8_t* volatile newBaseV = new uint8_t[m_liveDataSize];
    uint8_t* newBase = newBaseV;
    if (!newBase) return;  
    __builtin_memcpy(newBase, oldBase, m_liveDataSize);

    intptr_t delta = reinterpret_cast<intptr_t>(newBase) - reinterpret_cast<intptr_t>(oldBase);

    auto reloc = [delta](auto* ptr) -> decltype(ptr) {
        if (!ptr) return ptr;
        return reinterpret_cast<decltype(ptr)>(reinterpret_cast<intptr_t>(ptr) + delta);
    };

    for (auto& go : m_gameObjects) {
        go = reloc(go);
        go->polygons = reloc(go->polygons);
    }
    for (auto& lf : m_luaFiles) {
        lf = reloc(lf);
        lf->luaCode = reloc(lf->luaCode);
    }
    for (auto& name : m_objectNames) name = reloc(name);
    for (auto& name : m_audioClipNames) name = reloc(name);
    for (auto& inter : m_interactables) inter = reloc(inter);

    m_bvh.relocate(delta);
    m_navRegions.relocate(delta);

    m_rooms = reloc(m_rooms);
    m_portals = reloc(m_portals);
    m_roomTriRefs = reloc(m_roomTriRefs);
    m_roomCells = reloc(m_roomCells);
    m_roomPortalRefs = reloc(m_roomPortalRefs);

    for (int ci = 0; ci < m_cutsceneCount; ci++) {
        auto& cs = m_cutscenes[ci];
        cs.name = reloc(cs.name);
        cs.audioEvents = reloc(cs.audioEvents);
        for (uint8_t ti = 0; ti < cs.trackCount; ti++) {
            auto& track = cs.tracks[ti];
            track.keyframes = reloc(track.keyframes);
            if (track.target) track.target = reloc(track.target);
        }
    }

    for (int ai = 0; ai < m_animationCount; ai++) {
        auto& an = m_animations[ai];
        an.name = reloc(an.name);
        for (uint8_t ti = 0; ti < an.trackCount; ti++) {
            auto& track = an.tracks[ti];
            track.keyframes = reloc(track.keyframes);
            if (track.target) track.target = reloc(track.target);
        }
    }

    for (int si = 0; si < m_skinnedMeshCount; si++) {
        auto& ss = m_skinAnimSets[si];
        ss.boneIndices = reloc(ss.boneIndices);
        for (uint8_t ci = 0; ci < ss.clipCount; ci++) {
            ss.clips[ci].name = reloc(ss.clips[ci].name);
            ss.clips[ci].frames = reloc(ss.clips[ci].frames);
        }
    }

    m_uiSystem.relocate(delta);

    if (!m_gameObjects.empty()) {
        L.RelocateGameObjects(
            reinterpret_cast<GameObject**>(m_gameObjects.data()),
            m_gameObjects.size(), delta);
    }

    FileLoader::Get().FreeFile(oldBase);
    m_currentSceneData = newBase;
}

// ============================================================================
// OBJECT NAME LOOKUP
// ============================================================================


psxsplash::GameObject* psxsplash::SceneManager::findObjectByName(const char* name) const {
    if (!name || m_objectNames.empty()) return nullptr;
    for (size_t i = 0; i < m_objectNames.size() && i < m_gameObjects.size(); i++) {
        if (m_objectNames[i] && streq(m_objectNames[i], name)) {
            return m_gameObjects[i];
        }
    }
    return nullptr;
}

int psxsplash::SceneManager::findAudioClipByName(const char* name) const {
    if (!name || m_audioClipNames.empty()) return -1;
    for (size_t i = 0; i < m_audioClipNames.size(); i++) {
        if (m_audioClipNames[i] && streq(m_audioClipNames[i], name)) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

int psxsplash::SceneManager::findSkinAnimByObjectName(const char* name) const {
    if (!name || m_objectNames.empty()) return -1;
    for (size_t i = 0; i < m_objectNames.size() && i < m_gameObjects.size(); i++) {
        if (m_objectNames[i] && streq(m_objectNames[i], name)) {
            for (int si = 0; si < m_skinnedMeshCount; si++) {
                if (m_skinAnimSets[si].gameObjectIndex == (uint16_t)i) return si;
            }
            return -1;  // Object found but not skinned
        }
    }
    return -1;
}
