#pragma once

#include <EASTL/vector.h>

#include <psyqo/trigonometry.hh>
#include <psyqo/vector.hh>
#include <psyqo/gpu.hh>

#include "bvh.hh"
#include "camera.hh"
#include "collision.hh"
#include "controls.hh"
#include "gameobject.hh"
#include "lua.h"
#include "splashpack.hh"
#include "navregion.hh"
#include "audiomanager.hh"
#include "interactable.hh"
#include "luaapi.hh"
#include "fileloader.hh"
#include "cutscene.hh"
#include "animation.hh"
#include "uisystem.hh"
#ifdef PSXSPLASH_MEMOVERLAY
#include "memoverlay.hh"
#endif

namespace psxsplash {

// Forward-declare; full definition in loadingscreen.hh
class LoadingScreen;

class SceneManager {
  public:
    void InitializeScene(uint8_t* splashpackData, LoadingScreen* loading = nullptr);
    void GameTick(psyqo::GPU &gpu);
    
    // Font access (set from main.cpp after uploadSystemFont)
    static void SetFont(psyqo::Font<>* font) { s_font = font; }
    static psyqo::Font<>* GetFont() { return s_font; }
    
    // Trigger event callbacks (called by CollisionSystem for trigger boxes)
    void fireTriggerEnter(int16_t luaFileIndex, uint16_t triggerIndex);
    void fireTriggerExit(int16_t luaFileIndex, uint16_t triggerIndex);
    
    // Get game object by index (for collision callbacks)
    GameObject* getGameObject(uint16_t index) {
        if (index < m_gameObjects.size()) return m_gameObjects[index];
        return nullptr;
    }
    
    // Get total object count
    size_t getGameObjectCount() const { return m_gameObjects.size(); }
    
    // Get object name by index (returns nullptr if no name table or out of range)
    const char* getObjectName(uint16_t index) const {
        if (index < m_objectNames.size()) return m_objectNames[index];
        return nullptr;
    }
    
    // Find first object with matching name (linear scan, case-sensitive)
    GameObject* findObjectByName(const char* name) const;
    
    // Find audio clip index by name (returns -1 if not found)
    int findAudioClipByName(const char* name) const;
    
    // Get audio clip name by index (returns nullptr if out of range)
    const char* getAudioClipName(int index) const {
        if (index >= 0 && index < (int)m_audioClipNames.size()) return m_audioClipNames[index];
        return nullptr;
    }
    
    // Public API for game systems
    // Interaction system - call from Lua or native code
    void triggerInteraction(GameObject* interactable);
    
    // GameObject state control with events
    void setObjectActive(GameObject* go, bool active);
    
    // Public accessors for Lua API
    Controls& getControls() { return m_controls; }
    Camera& getCamera() { return m_currentCamera; }
    Lua& getLua() { return L; }
    AudioManager& getAudio() { return m_audio; }

    // Controls enable/disable (Lua-driven)
    void setControlsEnabled(bool enabled) { m_controlsEnabled = enabled; }
    bool isControlsEnabled() const { return m_controlsEnabled; }

    // Interactable access (for Lua API)
    Interactable* getInteractable(uint16_t index) {
        if (index < m_interactables.size()) return m_interactables[index];
        return nullptr;
    }

    // Player
    psyqo::Vec3& getPlayerPosition();
    void setPlayerPosition(psyqo::FixedPoint<12> x, psyqo::FixedPoint<12> y, psyqo::FixedPoint<12> z);
    
    // Scene loading (for multi-scene support)
    void requestSceneLoad(int sceneIndex);
    int getCurrentSceneIndex() const { return m_currentSceneIndex; }

    /// Load a scene by index.  This is the ONE canonical load path used by
    /// both the initial boot (main.cpp) and runtime scene transitions.
    /// Blanks the screen, shows a loading screen, tears down the old scene,
    /// loads the new splashpack, and initialises.
    /// @param gpu          GPU reference.
    /// @param sceneIndex    Scene to load.
    /// @param isFirstScene  True when called from boot (skips clearScene / free).
    void loadScene(psyqo::GPU& gpu, int sceneIndex, bool isFirstScene = false);

    // Check and process pending scene load (called from GameTick)
    void processPendingSceneLoad();

  private:
    psxsplash::Lua L;
    psxsplash::SplashPackLoader m_loader;
    CollisionSystem m_collisionSystem;
    BVHManager m_bvh;  // Spatial acceleration for frustum culling
    NavRegionSystem m_navRegions;      // Convex region navigation (v7+)
    uint16_t m_playerNavRegion = NAV_NO_REGION; // Current nav region for player
    
    // Scene type and render path: 0=exterior (BVH), 1=interior (room/portal)
    uint16_t m_sceneType = 0;
    
    // Room/portal data (v11+ interior scenes). Pointers into splashpack data.
    const RoomData* m_rooms = nullptr;
    uint16_t m_roomCount = 0;
    const PortalData* m_portals = nullptr;
    uint16_t m_portalCount = 0;
    const TriangleRef* m_roomTriRefs = nullptr;
    uint16_t m_roomTriRefCount = 0;

    eastl::vector<LuaFile*> m_luaFiles;
    eastl::vector<GameObject*> m_gameObjects;
    
    // Object name table (v9+): parallel to m_gameObjects, points into splashpack data
    eastl::vector<const char*> m_objectNames;
    
    // Audio clip name table (v10+): parallel to audio clips, points into splashpack data
    eastl::vector<const char*> m_audioClipNames;
    
    // Component arrays
    eastl::vector<Interactable*> m_interactables;
    
    // Audio system
    AudioManager m_audio;
    
    // Cutscene playback
    Cutscene m_cutscenes[MAX_CUTSCENES];
    int m_cutsceneCount = 0;
    CutscenePlayer m_cutscenePlayer;

    Animation m_animations[MAX_ANIMATIONS];
    int m_animationCount = 0;
    AnimationPlayer m_animationPlayer;
    
    UISystem m_uiSystem;
#ifdef PSXSPLASH_MEMOVERLAY
    MemOverlay m_memOverlay;
#endif
    
    psxsplash::Controls m_controls;

    psxsplash::Camera m_currentCamera;

    psyqo::Vec3 m_playerPosition;
    psyqo::Angle playerRotationX, playerRotationY, playerRotationZ;

    psyqo::FixedPoint<12, uint16_t> m_playerHeight;
    
    int32_t m_playerRadius;          
    int32_t m_velocityY;             
    int32_t m_gravityPerFrame;        
    int32_t m_jumpVelocityRaw;        
    bool m_isGrounded;                
    
    // Frame timing
    uint32_t m_lastFrameTime;         // gpu.now() timestamp of previous frame
    int m_deltaFrames;                // Elapsed frame count (1 normally, 2+ if dropped)

    bool freecam = false;
    bool m_controlsEnabled = true;    // Lua can disable all player input
    bool m_cameraFollowsPlayer = true; // False when scene has no nav regions (freecam/cutscene mode)
    
    // Static font pointer (set from main.cpp)
    static psyqo::Font<>* s_font;
    
    // Scene transition state
    int m_currentSceneIndex = 0;
    int m_pendingSceneIndex = -1;        // -1 = no pending load
    uint8_t* m_currentSceneData = nullptr; // Owned pointer to loaded data
    uint32_t m_liveDataSize = 0;         // Bytes of m_currentSceneData still needed at runtime
    
    // System update methods (called from GameTick)
    void updateInteractionSystem();
    void processEnableDisableEvents();
    void clearScene();  // Deallocate current scene objects
    void shrinkBuffer(); // Free pixel/audio bulk data after VRAM/SPU uploads
};
}  // namespace psxsplash