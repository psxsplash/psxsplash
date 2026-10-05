#pragma once

#include <EASTL/vector.h>

#include <psyqo/trigonometry.hh>
#include <psyqo/vector.hh>
#include <psyqo/gpu.hh>
#include <psyqo/soft-math.hh>

#include "features.hh"
#include "random.hh"
#include "gtemath.hh"
#include "bvh.hh"
#include "camera.hh"
#include "collision.hh"
#include "controls.hh"
#include "gameobject.hh"
#include "lua.h"
#include "splashpack.hh"
#include "navregion.hh"
#include "audiomanager.hh"
#include "musicmanager.hh"
#include "worldstreamer.hh"
#include "interactable.hh"
#include "luaapi.hh"
#include "fileloader.hh"
#include "cutscene.hh"
#include "animation.hh"
#include "skinmesh.hh"
#include "spritesystem.hh"
#include "tilesystem.hh"
#include "uisystem.hh"
#ifdef PSXSPLASH_MEMOVERLAY
#include "memoverlay.hh"
#endif

namespace psxsplash {

    // Forward-declare; full definition in loadingscreen.hh
    class LoadingScreen;

    class SceneManager {
    public:
        static constexpr uint16_t PLAYER_ACTOR_ID = 0;

        /// Agent state machine states.  Matches the Lua integer constants exposed
        /// via Agent.GetState / Agent.SetState.
        enum AgentState : uint8_t {
            AGENT_STATE_IDLE        = 0,
            AGENT_STATE_PATROL      = 1,
            AGENT_STATE_SEEK        = 2,
            AGENT_STATE_FLEE        = 3,
            AGENT_STATE_ATTACK      = 4,
            AGENT_STATE_WANDER      = 5,
            AGENT_STATE_INVESTIGATE = 6,
            AGENT_STATE_CUSTOM      = 7,
            AGENT_STATE_COUNT       = 8,
        };

        void InitializeScene(uint8_t* splashpackData, LoadingScreen* loading = nullptr);
        void GameTick(psyqo::GPU& gpu);

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

        // Actor access (player + object-backed actors).
        // Actor id space: 0 = player (PLAYER_ACTOR_ID); objectIndex + 1 = object-backed actors.
        size_t getActorCount() const { return m_gameObjects.size() + 1; }
        bool isPlayerActor(uint16_t actorId) const { return actorId == PLAYER_ACTOR_ID; }
        bool isValidActor(uint16_t actorId) const {
            if (actorId == PLAYER_ACTOR_ID) return true;
            uint16_t objectIndex = actorId - 1;
            return objectIndex < m_gameObjects.size() && m_gameObjects[objectIndex] != nullptr;
        }
        GameObject* getActorGameObject(uint16_t actorId) const {
            if (actorId == PLAYER_ACTOR_ID || actorId == 0xFFFF) return nullptr;
            uint16_t objectIndex = actorId - 1;
            if (objectIndex < m_gameObjects.size()) return m_gameObjects[objectIndex];
            return nullptr;
        }
        uint16_t findActorByName(const char* name) const {
            auto namesEqual = [](const char* a, const char* b) {
                if (a == b) return true;
                if (!a || !b) return false;
                while (*a && *b) {
                    if (*a != *b) return false;
                    ++a;
                    ++b;
                }
                return *a == *b;
            };

            if (!name) return 0xFFFF;
            if (namesEqual(name, "player")) return PLAYER_ACTOR_ID;

            GameObject* go = findObjectByName(name);
            if (!go) return 0xFFFF;

            for (uint16_t i = 0; i < m_gameObjects.size(); ++i) {
                if (m_gameObjects[i] == go) return static_cast<uint16_t>(i + 1);
            }
            return 0xFFFF;
        }
        const char* getActorName(uint16_t actorId) const {
            if (actorId == PLAYER_ACTOR_ID) return "player";
            uint16_t objectIndex = actorId - 1;
            if (objectIndex < m_objectNames.size()) return m_objectNames[objectIndex];
            return nullptr;
        }
        bool getActorPosition(uint16_t actorId, psyqo::Vec3& outPosition) const {
            if (actorId == PLAYER_ACTOR_ID) {
                outPosition = m_playerPosition;
                return true;
            }

            GameObject* go = getActorGameObject(actorId);
            if (!go) return false;
            outPosition = go->position;
            return true;
        }
        bool setActorPosition(uint16_t actorId, const psyqo::Vec3& position) {
            if (actorId == PLAYER_ACTOR_ID) {
                setPlayerPosition(position.x, position.y, position.z);
                return true;
            }

            GameObject* go = getActorGameObject(actorId);
            if (!go) return false;

            int32_t dx = position.x.value - go->position.x.value;
            int32_t dy = position.y.value - go->position.y.value;
            int32_t dz = position.z.value - go->position.z.value;

            go->position = position;
            go->aabbMinX += dx; go->aabbMaxX += dx;
            go->aabbMinY += dy; go->aabbMaxY += dy;
            go->aabbMinZ += dz; go->aabbMaxZ += dz;
            go->setDynamicMoved(true);
            return true;
        }
        bool getActorRotation(uint16_t actorId, psyqo::Vec3& outRotation) const {
            if (actorId == PLAYER_ACTOR_ID) {
                outRotation.x = static_cast<psyqo::FixedPoint<12>>(playerRotationX);
                outRotation.y = static_cast<psyqo::FixedPoint<12>>(playerRotationY);
                outRotation.z = static_cast<psyqo::FixedPoint<12>>(playerRotationZ);
                return true;
            }

            GameObject* go = getActorGameObject(actorId);
            if (!go) return false;

            auto fastAtan2 = [](int32_t sinVal, int32_t cosVal) {
                psyqo::Angle result;
                if (cosVal == 0 && sinVal == 0) {
                    result.value = 0;
                    return result;
                }

                int32_t absS = sinVal < 0 ? -sinVal : sinVal;
                int32_t absC = cosVal < 0 ? -cosVal : cosVal;
                int32_t minV = absS < absC ? absS : absC;
                int32_t maxV = absS > absC ? absS : absC;
                int32_t angle = (minV * 256) / maxV;

                if (absS > absC) angle = 512 - angle;
                if (cosVal < 0) angle = 1024 - angle;
                if (sinVal < 0) angle = -angle;

                result.value = angle;
                return result;
            };

            outRotation.x.value = 0;
            outRotation.z.value = 0;
            psyqo::Angle angleY = fastAtan2(go->rotation.vs[0].z.raw(), go->rotation.vs[0].x.raw());
            outRotation.y = static_cast<psyqo::FixedPoint<12>>(angleY);
            return true;
        }
        bool setActorRotation(uint16_t actorId, const psyqo::Vec3& rotation) {
            if (actorId == PLAYER_ACTOR_ID) {
                setPlayerRotation(rotation.x, rotation.y, rotation.z);
                return true;
            }

            GameObject* go = getActorGameObject(actorId);
            if (!go) return false;

            psyqo::Angle rx = static_cast<psyqo::Angle>(rotation.x);
            psyqo::Angle ry = static_cast<psyqo::Angle>(rotation.y);
            psyqo::Angle rz = static_cast<psyqo::Angle>(rotation.z);
            static psyqo::Trig<> trig;
            auto matY = psyqo::SoftMath::generateRotationMatrix33(ry, psyqo::SoftMath::Axis::Y, trig);
            auto matX = psyqo::SoftMath::generateRotationMatrix33(rx, psyqo::SoftMath::Axis::X, trig);
            auto matZ = psyqo::SoftMath::generateRotationMatrix33(rz, psyqo::SoftMath::Axis::Z, trig);
            auto temp = psyqo::SoftMath::multiplyMatrix33(matY, matX);
            go->rotation = psxsplash::transposeMatrix33(psyqo::SoftMath::multiplyMatrix33(temp, matZ));
            return true;
        }
        bool isNavLoaded() const { return m_navRegions.isLoaded(); }
        uint16_t getActorNavRegion(uint16_t actorId) const {
            if (!m_navRegions.isLoaded()) return NAV_NO_REGION;
            if (actorId == PLAYER_ACTOR_ID) return m_playerNavRegion;

            psyqo::Vec3 actorPosition;
            if (!getActorPosition(actorId, actorPosition)) return NAV_NO_REGION;
            return m_navRegions.findRegionNearest(actorPosition.x.value, actorPosition.y.value, actorPosition.z.value);
        }
        bool getNavRegionCenter(uint16_t regionIndex, psyqo::Vec3& outPosition) const {
            int32_t x = 0;
            int32_t y = 0;
            int32_t z = 0;
            if (!m_navRegions.getRegionCenter(regionIndex, x, y, z)) return false;

            outPosition.x.value = x;
            outPosition.y.value = y;
            outPosition.z.value = z;
            return true;
        }
        bool findActorPath(uint16_t actorId, uint16_t targetActorId, NavPath& outPath) const {
            psyqo::Vec3 targetPosition;
            if (!getActorPosition(targetActorId, targetPosition)) {
                outPath.stepCount = 0;
                return false;
            }
            return findActorPathToPosition(actorId, targetPosition, outPath);
        }
        bool findActorPathToPosition(uint16_t actorId, const psyqo::Vec3& targetPosition, NavPath& outPath) const {
            outPath.stepCount = 0;
            if (!m_navRegions.isLoaded()) return false;

            uint16_t startRegion = getActorNavRegion(actorId);
            uint16_t endRegion = m_navRegions.findRegionNearest(targetPosition.x.value, targetPosition.y.value, targetPosition.z.value);
            if (startRegion == NAV_NO_REGION || endRegion == NAV_NO_REGION) return false;

            return m_navRegions.findPath(startRegion, endRegion, outPath);
        }

        bool isActorAgent(uint16_t actorId) const {
            return actorId < m_agentStates.size() && (m_agentStates[actorId].flags & AGENT_FLAG_REGISTERED) != 0;
        }
        bool isActorAgentEnabled(uint16_t actorId) const {
            return actorId < m_agentStates.size() && (m_agentStates[actorId].flags & AGENT_FLAG_ENABLED) != 0;
        }
        bool isActorMoving(uint16_t actorId) const {
            return actorId < m_agentStates.size() && (m_agentStates[actorId].flags & AGENT_FLAG_MOVING) != 0;
        }
        bool setActorAgentEnabled(uint16_t actorId, bool enabled) {
            if (!isValidActor(actorId) || actorId >= m_agentStates.size()) return false;

            auto& agent = m_agentStates[actorId];
            agent.flags |= AGENT_FLAG_REGISTERED;
            if (enabled) agent.flags |= AGENT_FLAG_ENABLED;
            else {
                agent.flags &= ~(AGENT_FLAG_ENABLED | AGENT_FLAG_MOVING | AGENT_FLAG_TARGET_ACTOR);
                agent.path.stepCount = 0;
                agent.currentPathIndex = 0;
                agent.targetActorId = 0xFFFF;
            }

            if (agent.moveSpeed.value == 0) agent.moveSpeed = m_defaultAgentMoveSpeed;
            if (agent.stopDistance.value == 0) agent.stopDistance = m_defaultAgentStopDistance;
            return true;
        }
        bool moveActorToPosition(uint16_t actorId, const psyqo::Vec3& targetPosition) {
            if (!setActorAgentEnabled(actorId, true)) return false;

            auto& agent = m_agentStates[actorId];
            agent.flags &= ~AGENT_FLAG_TARGET_ACTOR;
            agent.flags |= AGENT_FLAG_MOVING;
            agent.targetActorId = 0xFFFF;
            agent.targetPosition = targetPosition;
            agent.repathCounter = 0;
            return rebuildAgentPath(actorId, agent);
        }
        bool moveActorToActor(uint16_t actorId, uint16_t targetActorId) {
            if (!isValidActor(targetActorId)) return false;
            if (!setActorAgentEnabled(actorId, true)) return false;

            auto& agent = m_agentStates[actorId];
            agent.flags |= AGENT_FLAG_MOVING | AGENT_FLAG_TARGET_ACTOR;
            agent.targetActorId = targetActorId;
            agent.repathCounter = 0;
            return rebuildAgentPath(actorId, agent);
        }
        void stopActor(uint16_t actorId) {
            if (actorId >= m_agentStates.size()) return;

            auto& agent = m_agentStates[actorId];
            agent.flags &= ~(AGENT_FLAG_MOVING | AGENT_FLAG_TARGET_ACTOR);
            agent.currentPathIndex = 0;
            agent.repathCounter = 0;
            agent.targetActorId = 0xFFFF;
            agent.path.stepCount = 0;
        }
        bool setActorMoveSpeed(uint16_t actorId, psyqo::FixedPoint<12> speed) {
            if (!isValidActor(actorId) || actorId >= m_agentStates.size()) return false;
            auto& agent = m_agentStates[actorId];
            agent.flags |= AGENT_FLAG_REGISTERED;
            agent.moveSpeed.value = speed.value > 0 ? static_cast<uint16_t>(speed.value) : 0;
            return true;
        }
        psyqo::FixedPoint<12> getActorMoveSpeed(uint16_t actorId) const {
            psyqo::FixedPoint<12> speed;
            if (actorId >= m_agentStates.size()) return speed;
            speed.value = m_agentStates[actorId].moveSpeed.value;
            return speed;
        }
        bool getActorTarget(uint16_t actorId, uint16_t& outTargetActor) const {
            outTargetActor = 0xFFFF;
            if (actorId >= m_agentStates.size()) return false;
            const auto& agent = m_agentStates[actorId];
            if ((agent.flags & AGENT_FLAG_TARGET_ACTOR) == 0 || !isValidActor(agent.targetActorId)) return false;
            outTargetActor = agent.targetActorId;
            return true;
        }

        // ---- State machine API ----
        AgentState getActorAgentState(uint16_t actorId) const {
            if (actorId >= m_agentStates.size()) return AGENT_STATE_IDLE;
            return m_agentStates[actorId].currentState;
        }
        void setActorAgentState(uint16_t actorId, AgentState newState);

        // ---- Vision / hearing config API ----
        void setActorVisionRange(uint16_t actorId, psyqo::FixedPoint<12> range) {
            if (actorId >= m_agentStates.size()) return;
            auto& ag = m_agentStates[actorId];
            ag.visionRange.value = static_cast<uint16_t>(range.value > 0 ? range.value : 0);
            if (ag.visionRange.value > 0) ag.flags |= AGENT_FLAG_HAS_VISION;
            else ag.flags &= ~AGENT_FLAG_HAS_VISION;
        }
        psyqo::FixedPoint<12> getActorVisionRange(uint16_t actorId) const {
            psyqo::FixedPoint<12> r; r.value = 0;
            if (actorId < m_agentStates.size()) r.value = m_agentStates[actorId].visionRange.value;
            return r;
        }
        void setActorVisionAngleCos(uint16_t actorId, int16_t cosAngleFP12) {
            if (actorId < m_agentStates.size()) m_agentStates[actorId].visionCosAngle = cosAngleFP12;
        }
        void setActorHearingRange(uint16_t actorId, psyqo::FixedPoint<12> range) {
            if (actorId >= m_agentStates.size()) return;
            auto& ag = m_agentStates[actorId];
            ag.hearingRange.value = static_cast<uint16_t>(range.value > 0 ? range.value : 0);
            if (ag.hearingRange.value > 0) ag.flags |= AGENT_FLAG_HAS_HEARING;
            else ag.flags &= ~AGENT_FLAG_HAS_HEARING;
        }
        psyqo::FixedPoint<12> getActorHearingRange(uint16_t actorId) const {
            psyqo::FixedPoint<12> r; r.value = 0;
            if (actorId < m_agentStates.size()) r.value = m_agentStates[actorId].hearingRange.value;
            return r;
        }
        void setActorAlertTimeout(uint16_t actorId, uint16_t frames) {
            if (actorId < m_agentStates.size()) m_agentStates[actorId].alertTimeoutFrames = frames;
        }

        // ---- Vision check (public for Lua CanSee) ----
        bool canActorSeeActor(uint16_t observerActorId, uint16_t targetActorId) const;
        bool canActorHearActor(uint16_t observerActorId, uint16_t targetActorId) const;

        // ---- Last known position ----
        bool getActorLastKnownPos(uint16_t actorId, psyqo::Vec3& outPos) const {
            if (actorId >= m_agentStates.size()) return false;
            const auto& ag = m_agentStates[actorId];
            // Only valid if target was ever seen
            if ((ag.flags & AGENT_FLAG_HAS_VISION) == 0) return false;
            outPos = ag.lastKnownPos;
            return true;
        }

        // ---- Patrol waypoints ----
        void addActorWaypoint(uint16_t actorId, const psyqo::Vec3& wp) {
            if (actorId >= m_agentStates.size()) return;
            auto& ag = m_agentStates[actorId];
            if (ag.waypointCount >= 8) return;
            ag.waypoints[ag.waypointCount++] = wp;
            if (ag.waypointCount > 0) ag.flags |= AGENT_FLAG_HAS_PATROL;
        }
        void clearActorWaypoints(uint16_t actorId) {
            if (actorId >= m_agentStates.size()) return;
            auto& ag = m_agentStates[actorId];
            ag.waypointCount = 0;
            ag.waypointIndex = 0;
            ag.flags &= ~AGENT_FLAG_HAS_PATROL;
        }
        void setActorPatrolEnabled(uint16_t actorId, bool enabled) {
            if (actorId >= m_agentStates.size()) return;
            if (enabled) m_agentStates[actorId].flags |= AGENT_FLAG_HAS_PATROL;
            else m_agentStates[actorId].flags &= ~AGENT_FLAG_HAS_PATROL;
        }

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

#if PSXSPLASH_FEATURE_SKIN
        // Skinned mesh accessors (for Lua API and renderer)
        int findSkinAnimByObjectName(const char* name) const;
        SkinAnimSet& getSkinAnimSet(int index) { return m_skinAnimSets[index]; }
        SkinAnimState& getSkinAnimState(int index) { return m_skinAnimStates[index]; }
        int getSkinnedMeshCount() const { return m_skinnedMeshCount; }
#endif

        // Point lights (for the Lua API). The renderer reads the same array.
        // Without the lights feature there are none, and anything that looks
        // one up (Light.*, light tracks) gets nullptr and skips it.
#if PSXSPLASH_FEATURE_LIGHTS
        int findPointLight(const char* name) const;
#endif
        PointLight* getPointLight(int index) {
#if PSXSPLASH_FEATURE_LIGHTS
            return (index >= 0 && index < m_pointLightCount) ? &m_pointLights[index] : nullptr;
#else
            (void)index;
            return nullptr;
#endif
        }

        // Public API for game systems
        // Interaction system - call from Lua or native code
        void triggerInteraction(GameObject* interactable);

        // GameObject state control with events
        void setObjectActive(GameObject* go, bool active);

        // Public accessors for Lua API
        Controls& getControlsPlayer1() { return m_controls[0]; }
        Controls& getControlsPlayer2() { return m_controls[1]; }
        Camera& getCamera() { return m_currentCamera; }
        Lua& getLua() { return L; }
        AudioManager& getAudio() { return m_audio; }
        MusicManager& getMusic() { return m_music; }

        // Controls enable/disable (Lua-driven)
        void setControlsEnabledPlayer1(bool enabled) { m_controlsEnabled[0] = enabled; }
        bool isControlsEnabledPlayer1() const { return m_controlsEnabled[0]; }

        void setControlsEnabledPlayer2(bool enabled) { m_controlsEnabled[1] = enabled; }
        bool isControlsEnabledPlayer2() const { return m_controlsEnabled[1]; }

        // Actor-parented input: bind a controller (player 0/1) to any actor.
        // Default is the player actor, so binding to PLAYER_ACTOR_ID reproduces
        // the classic behaviour (full locomotion: gravity/jump/nav). Binding to
        // another actor drives that actor's transform directly.
        void setControlBoundActor(int player, uint16_t actorId) {
            if (player >= 0 && player < 2) m_controlBoundActor[player] = actorId;
        }
        uint16_t getControlBoundActor(int player) const {
            return (player >= 0 && player < 2) ? m_controlBoundActor[player] : PLAYER_ACTOR_ID;
        }

        // enable/disable (Lua-driven)
        void setCameraFollowPlayer(bool enabled) { m_cameraFollowsPlayer = enabled; }
        void setCameraFollowActor(uint16_t actorId) {
            if (isValidActor(actorId)) {
                m_cameraFollowActor = actorId;
                m_cameraFollowsPlayer = true;
            }
        }
        uint16_t getCameraFollowActor() const { return m_cameraFollowActor; }

        // Interactable access (for Lua API)
        Interactable* getInteractable(uint16_t index) {
            if (index < m_interactables.size()) return m_interactables[index];
            return nullptr;
        }

        // Player
        psyqo::Vec3& getPlayerPosition();
        void setPlayerPosition(psyqo::FixedPoint<12> x, psyqo::FixedPoint<12> y, psyqo::FixedPoint<12> z);
        psyqo::Vec3 getPlayerRotation();
        void setPlayerRotation(psyqo::FixedPoint<12> x, psyqo::FixedPoint<12> y, psyqo::FixedPoint<12> z);

        // Scene loading (for multi-scene support)
        void requestSceneLoad(int sceneIndex);
        int getCurrentSceneIndex() const { return m_currentSceneIndex; }

        /// Authored network scene id from the splashpack (v22+), or 0 when the
        /// pack predates it / the exporter left it unset. 0 means the caller
        /// should fall back to the derived hash.
        uint32_t getAuthoredSceneHash() const { return m_authoredSceneHash; }

        // Most recent gpu.now() timestamp (a free-running hardware timer sample).
        // Differs across independently-booted consoles, so it seeds the network
        // host-election tiebreak.
        uint32_t getFrameTimestamp() const { return m_lastFrameTime; }

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

        static Random m_random;
        static Random m_randomGenerator;

    private:
        psxsplash::Lua L;
        psxsplash::SplashPackLoader m_loader;
#if PSXSPLASH_FEATURE_COLLISION
        CollisionSystem m_collisionSystem;
#endif
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
        const RoomCell* m_roomCells = nullptr;
        uint16_t m_roomCellCount = 0;
        const RoomPortalRef* m_roomPortalRefs = nullptr;
        uint16_t m_roomPortalRefCount = 0;

        eastl::vector<LuaFile*> m_luaFiles;
        eastl::vector<GameObject*> m_gameObjects;

        // Object name table (v9+): parallel to m_gameObjects, points into splashpack data
        eastl::vector<const char*> m_objectNames;

        // Audio clip name table (v10+): parallel to audio clips, points into splashpack data
        eastl::vector<const char*> m_audioClipNames;

        // Component arrays
        eastl::vector<Interactable*> m_interactables;
        eastl::vector<Interactable> m_legacyInteractables;  // pre-v27 packs only

        // Audio system
        AudioManager m_audio;
        MusicManager m_music;
#if PSXSPLASH_FEATURE_STREAMING
        WorldStreamer m_worldStreamer;
#endif

#if PSXSPLASH_FEATURE_CUTSCENE
        // Cutscene playback
        Cutscene m_cutscenes[MAX_CUTSCENES];
        int m_cutsceneCount = 0;
        CutscenePlayer m_cutscenePlayer;

        Animation m_animations[MAX_ANIMATIONS];
        int m_animationCount = 0;
        AnimationPlayer m_animationPlayer;
#endif

#if PSXSPLASH_FEATURE_SKIN
        SkinAnimSet  m_skinAnimSets[MAX_SKINNED_MESHES];
        SkinAnimState m_skinAnimStates[MAX_SKINNED_MESHES];
        int m_skinnedMeshCount = 0;
#endif

#if PSXSPLASH_FEATURE_UI
        UISystem m_uiSystem;
#endif
#if PSXSPLASH_FEATURE_SPRITES
        SpriteSystem m_spriteSystem;
        TileSystem m_tileSystem;
#endif
        uint32_t m_authoredSceneHash = 0;

#if PSXSPLASH_FEATURE_LIGHTS
        PointLight m_pointLights[MAX_SCENE_LIGHTS];
        const char* m_pointLightNames[MAX_SCENE_LIGHTS];
        int m_pointLightCount = 0;
#endif
#ifdef PSXSPLASH_MEMOVERLAY
        MemOverlay m_memOverlay;
#endif

        psxsplash::Controls m_controls[2];

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
        int32_t m_dt12;                   // Frame delta in 4.12 fixed-point (4096 = one 30fps frame)

        bool freecam = false;
        bool m_controlsEnabled[2] = { true, true };    // Lua can disable all player input
        uint16_t m_controlBoundActor[2] = { PLAYER_ACTOR_ID, PLAYER_ACTOR_ID }; // actor each controller drives
        bool m_cameraFollowsPlayer = true; // False when scene has no nav regions (freecam/cutscene mode)
        uint16_t m_cameraFollowActor = PLAYER_ACTOR_ID; // Which actor the camera follows (default: player)

        // Static font pointer (set from main.cpp)
        static psyqo::Font<>* s_font;

        // Scene transition state
        int m_currentSceneIndex = 0;
        int m_pendingSceneIndex = -1;        // -1 = no pending load
        uint8_t* m_currentSceneData = nullptr; // Owned pointer to loaded splashpack data

        // ---- Agent runtime (native AI: pathing, perception, state machine) ----
        static constexpr uint8_t AGENT_FLAG_REGISTERED    = 0x01;
        static constexpr uint8_t AGENT_FLAG_ENABLED       = 0x02;
        static constexpr uint8_t AGENT_FLAG_MOVING        = 0x04;
        static constexpr uint8_t AGENT_FLAG_TARGET_ACTOR  = 0x08;
        static constexpr uint8_t AGENT_FLAG_HAS_VISION    = 0x10;
        static constexpr uint8_t AGENT_FLAG_HAS_HEARING   = 0x20;
        static constexpr uint8_t AGENT_FLAG_HAS_PATROL    = 0x40;
        static constexpr uint8_t AGENT_FLAG_TARGET_SEEN   = 0x80;
        static constexpr uint8_t AGENT_REPATH_INTERVAL    = 10;

        struct AgentRuntimeState {
            // ---- Movement core ----
            uint8_t  flags            = 0;
            uint8_t  currentPathIndex = 0;
            uint8_t  repathCounter    = 0;
            // ---- State machine ----
            AgentState currentState   = AGENT_STATE_IDLE;
            AgentState previousState  = AGENT_STATE_IDLE;
            // ---- Patrol ----
            uint8_t  waypointIndex    = 0;
            uint8_t  waypointCount    = 0;
            // ---- Vision throttle ----
            uint8_t  visionCheckCounter = 0;
            // ---- IDs ----
            uint16_t targetActorId    = 0xFFFF;
            // ---- Timers ----
            uint16_t stateTimer       = 0;   // frames in current state
            uint16_t alertCountdown   = 0;   // frames until onTargetLost fires
            // ---- Movement tuning ----
            psyqo::FixedPoint<12, uint16_t> moveSpeed    = 0;
            psyqo::FixedPoint<12, uint16_t> stopDistance = 0;
            // ---- Vision/hearing config ----
            psyqo::FixedPoint<12, uint16_t> visionRange  = 0;  // 0 = disabled
            int16_t  visionCosAngle   = 0;   // fp12 dot threshold (4096=0 deg, 0=90 deg, -4096=180 deg)
            psyqo::FixedPoint<12, uint16_t> hearingRange = 0;  // 0 = disabled
            uint16_t alertTimeoutFrames = 0;
            uint8_t  visionRegionDepth = 0;  // max nav region BFS hops for LOS
            // ---- Per-state animation clips ----
            uint8_t  stateAnimClip[AGENT_STATE_COUNT] = {};  // 0xFF = none
            // ---- Positions ----
            psyqo::Vec3 targetPosition  = {};
            psyqo::Vec3 lastKnownPos    = {};
            // ---- Pathfinding ----
            NavPath  path               = {};
            // ---- Patrol waypoints (up to 8) ----
            psyqo::Vec3 waypoints[8]    = {};
        };

        bool cutsceneDrivesCamera() const {
#if PSXSPLASH_FEATURE_CUTSCENE
            return m_cutscenePlayer.isPlaying() && m_cutscenePlayer.hasCameraTracks();
#else
            return false;
#endif
        }

        // System update methods (called from GameTick)
        void updateInteractionSystem();
        void processEnableDisableEvents();
        // Drive a non-player actor's transform from a controller (actor-parented input).
        void driveActorWithController(int controllerIndex, uint16_t actorId);
        void initializeAgentStates(const SplashpackSceneSetup& sceneSetup);
        void tickAgents();
        void tickAgentStateMachine(uint16_t actorId, AgentRuntimeState& agent);
        void tickAgentVisionHearing(uint16_t actorId, AgentRuntimeState& agent);
        void fireAgentEvent(uint16_t actorId, const char* eventName);
        void fireAgentEventWithArg(uint16_t actorId, const char* eventName, uint16_t argActorId);
        void fireAgentEventWithIndex(uint16_t actorId, const char* eventName, int argInt);
        void applyAgentStateAnimation(uint16_t actorId, AgentRuntimeState& agent, AgentState newState);
        bool rebuildAgentPath(uint16_t actorId, AgentRuntimeState& agent) {
            agent.path.stepCount = 0;
            agent.currentPathIndex = 0;

            if (!m_navRegions.isLoaded()) return false;

            if (agent.flags & AGENT_FLAG_TARGET_ACTOR) {
                if (!getActorPosition(agent.targetActorId, agent.targetPosition)) return false;
            }

            if (!findActorPathToPosition(actorId, agent.targetPosition, agent.path)) return false;
            agent.currentPathIndex = (agent.path.stepCount > 1) ? 1 : 0;
            return true;
        }
        bool getAgentWaypoint(uint16_t actorId, const AgentRuntimeState& agent, psyqo::Vec3& outWaypoint) const {
            (void)actorId;
            if ((agent.flags & AGENT_FLAG_MOVING) == 0 || agent.path.stepCount <= 0) return false;

            if (agent.currentPathIndex > 0 && agent.currentPathIndex < agent.path.stepCount - 1) {
                return getNavRegionCenter(agent.path.regions[agent.currentPathIndex], outWaypoint);
            }

            outWaypoint = agent.targetPosition;
            return true;
        }
        void clearScene();  // Deallocate current scene objects

        const uint16_t m_coyoteTimeDistance = 24; // How far you can fall and still jump
        const uint16_t m_downwardVelocityCap = 32; // Limit downward velocity

        // VRAM/SPU upload from separate data files (v20+)
        void uploadVramData(uint8_t* vramData, int vramSize);
        void uploadSpuData(uint8_t* spuData, int spuSize);

        // Agent runtime state, indexed by actorId (fixed-size, sized at scene load).
        eastl::vector<AgentRuntimeState> m_agentStates;
        psyqo::FixedPoint<12, uint16_t> m_defaultAgentMoveSpeed = 0;
        psyqo::FixedPoint<12, uint16_t> m_defaultAgentStopDistance = 0;
    };
}  // namespace psxsplash
