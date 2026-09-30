#pragma once

#include <stdint.h>

#include "gameobject.hh"
#include "psyqo-lua/lua.hh"
#include "psyqo/xprintf.h"
#include "typestring.h"

namespace psxsplash {

struct LuaFile {
    union {
        uint32_t luaCodeOffset;
        const char* luaCode;
    };
    uint32_t length;
};

/**
 * Event bitmask flags - each bit represents whether an object handles that event.
 * This allows O(1) checking before calling into Lua VM.
 * 
 * CRITICAL: The PS1 cannot afford to call into Lua for events objects don't handle.
 * When registering a GameObject, we scan its script and set these bits.
 * During dispatch, we check the bit FIRST before any Lua VM access.
 */
enum EventMask : uint32_t {
    EVENT_NONE              = 0,
    EVENT_ON_CREATE         = 1 << 0,
    EVENT_ON_COLLISION      = 1 << 1,
    EVENT_ON_INTERACT       = 1 << 2,
    EVENT_ON_TRIGGER_ENTER  = 1 << 3,
    EVENT_ON_TRIGGER_EXIT   = 1 << 4,
    EVENT_ON_UPDATE         = 1 << 5,
    EVENT_ON_DESTROY        = 1 << 6,
    EVENT_ON_ENABLE         = 1 << 7,
    EVENT_ON_DISABLE        = 1 << 8,
    EVENT_ON_BUTTON_PRESS   = 1 << 9,
    EVENT_ON_BUTTON_RELEASE = 1 << 10,
    // Agent state-machine events (bits 11-17)
    EVENT_ON_STATE_ENTER    = 1 << 11,
    EVENT_ON_STATE_EXIT     = 1 << 12,
    EVENT_ON_TARGET_SEEN    = 1 << 13,
    EVENT_ON_TARGET_LOST    = 1 << 14,
    EVENT_ON_TARGET_REACHED = 1 << 15,
    EVENT_ON_PATROL_POINT   = 1 << 16,
    EVENT_ON_PATH_BLOCKED   = 1 << 17,
};

class Lua {
  public:
    void Init();
    void Reset();     // Destroy and recreate the Lua VM (call on scene load)
    void Shutdown();  // Close the Lua VM without recreating (call on scene unload)

    void LoadLuaFile(const char* code, size_t len, int index);
    void RegisterSceneScripts(int index);
    void RegisterGameObject(GameObject* go, uint16_t actorId = 0xFFFF);
    void FireAllOnCreate(GameObject** objects, size_t count);
    void RelocateGameObjects(GameObject** objects, size_t count, intptr_t delta);
    
    // Get the underlying psyqo::Lua state for API registration
    psyqo::Lua& getState() { return m_state; }

    // Serialize a GameObject's `self.sync` table into buf (integer-only tagged
    // blob). Returns bytes written, or 0 if there is no sync table / on error.
    uint32_t SerializeObjectSync(GameObject* go, uint8_t* buf, uint32_t cap);
    // Deserialize a blob (from SerializeObjectSync) into a GameObject's
    // `self.sync` table. Returns true on success.
    bool ApplyObjectSync(GameObject* go, const uint8_t* buf, uint32_t size);
    
    /**
     * Check if a GameObject handles a specific event.
     * Call this BEFORE attempting to dispatch any event.
     */
    bool hasEvent(GameObject* go, EventMask event) const {
        return (go->eventMask & event) != 0;
    }

    void OnSceneCreationStart() {
        onSceneCreationStartFunctionWrapper.callFunction(*this);
    }
    void OnSceneCreationEnd() {
        onSceneCreationEndFunctionWrapper.callFunction(*this);
    }
    // Reliable network event delivered to the scene script (serial multiplayer).
    void OnNetEvent(int eventId, int arg) {
        onNetEventFunctionWrapper.callFunction(*this, eventId, arg);
    }
    // Reliable opaque application payload delivered to the scene script as a
    // binary string. The engine assigns it no meaning; games layer their own
    // protocol (lobbies, room lists, roles, votes) on top.
    void OnNetData(const uint8_t* data, uint16_t len) {
        onNetDataFunctionWrapper.callFunction(*this, LuaBytes{data, len});
    }
    
    // Event dispatchers - these check the bitmask before calling Lua    
    void OnCollideWithPlayer(GameObject* self);
    void OnInteract(GameObject* self);
    void OnTriggerEnter(GameObject* trigger, GameObject* other);
    void OnTriggerExit(GameObject* trigger, GameObject* other);
    void OnTriggerEnterScript(int luaFileIndex, int triggerIndex);
    void OnTriggerExitScript(int luaFileIndex, int triggerIndex);
    void OnUpdate(GameObject* go, int32_t dt12);  // Per-object update (4.12 fp, 4096 = 1 frame)
    void OnDestroy(GameObject* go);
    void OnEnable(GameObject* go);
    void OnDisable(GameObject* go);
    void OnButtonPress(GameObject* go, int button);
    void OnButtonRelease(GameObject* go, int button);

    // Agent state-machine event dispatchers
    // newState / oldState are integer AgentState values
    void OnAgentStateEnter(GameObject* go, int newState, int oldState);
    void OnAgentStateExit(GameObject* go, int state);
    void OnAgentTargetSeen(GameObject* go, GameObject* target);
    void OnAgentTargetLost(GameObject* go, GameObject* target);
    void OnAgentTargetReached(GameObject* go);
    void OnAgentPatrolPoint(GameObject* go, int waypointIndex);
    void OnAgentPathBlocked(GameObject* go);

  private:
    // A length-counted byte range destined for Lua. Lua strings are binary-safe
    // (counted, not NUL-terminated), so this carries arbitrary payloads -
    // including embedded zeros - through the same variadic pushArgs path as
    // every other argument type, with no special-casing at the call site.
    struct LuaBytes {
        const uint8_t* data;
        uint16_t len;
    };

    template <int methodId, typename methodName>
    struct FunctionWrapper;
    template <int methodId, char... C>
    struct FunctionWrapper<methodId, irqus::typestring<C...>> {
        typedef irqus::typestring<C...> methodName;
        
        // Returns true if the function was found and stored
        static bool resolveGlobal(psyqo::Lua L) {
            L.push(methodName::data(), methodName::size());  
            L.getTable(3);  
            
            if (L.isFunction(-1)) {
                L.pushNumber(methodId);
                L.copy(-2);
                L.setTable(1);
                L.pop(); // Pop the function
                return true;
            } else {
                L.pop();
                return false;
            }
        }
        
        template <typename... Args>
        static void pushArgs(psxsplash::Lua& lua, Args... args) {
            (push(lua, args), ...);
        }
        static void push(psxsplash::Lua& lua, GameObject* go) { lua.PushGameObject(go); }
        static void push(psxsplash::Lua& lua, int val) { lua.m_state.pushNumber(val); }
        static void push(psxsplash::Lua& lua, LuaBytes bytes) {
            lua.m_state.push(reinterpret_cast<const char*>(bytes.data), bytes.len);
        }
        
        template <typename... Args>
        static void callMethod(psxsplash::Lua& lua, GameObject* go, Args... args) {
            auto L = lua.m_state;
            uint8_t* ptr = reinterpret_cast<uint8_t*>(go);
            L.push(ptr + 1);
            L.rawGet(LUA_REGISTRYINDEX);
            L.rawGetI(-1, methodId);
            if (!L.isFunction(-1)) {
                L.clearStack();
                return;
            }
            lua.PushGameObject(go);
            pushArgs(lua, args...);
            if (L.pcall(sizeof...(Args) + 1, 0) != LUA_OK) {
                printf("Lua error: %s\n", L.toString(-1));
            }
            L.clearStack();
        }
        
        template <typename... Args>
        static void callFunction(psxsplash::Lua& lua, Args... args) {
            auto L = lua.m_state;
            L.rawGetI(LUA_REGISTRYINDEX, lua.m_luaSceneScriptsReference);
            if (!L.isTable(-1)) {
                L.clearStack();
                return;
            }
            L.rawGetI(-1, methodId);
            if (!L.isFunction(-1)) {
                L.clearStack();
                return;
            }
            pushArgs(lua, args...);
            if (L.pcall(sizeof...(Args), 0) != LUA_OK) {
                printf("Lua error: %s\n", L.toString(-1));
            }
            L.clearStack();
        }
    };

    // Scene-level events (methodId 1-4)
    [[no_unique_address]] FunctionWrapper<1, typestring_is("onSceneCreationStart")> onSceneCreationStartFunctionWrapper;
    [[no_unique_address]] FunctionWrapper<2, typestring_is("onSceneCreationEnd")> onSceneCreationEndFunctionWrapper;
    [[no_unique_address]] FunctionWrapper<3, typestring_is("onNetEvent")> onNetEventFunctionWrapper;
    [[no_unique_address]] FunctionWrapper<4, typestring_is("onNetData")> onNetDataFunctionWrapper;
    
    // Object-level events
    [[no_unique_address]] FunctionWrapper<100, typestring_is("onCreate")> onCreateMethodWrapper;
    [[no_unique_address]] FunctionWrapper<101, typestring_is("onCollideWithPlayer")> onCollideWithPlayerMethodWrapper;
    [[no_unique_address]] FunctionWrapper<102, typestring_is("onInteract")> onInteractMethodWrapper;
    [[no_unique_address]] FunctionWrapper<103, typestring_is("onTriggerEnter")> onTriggerEnterMethodWrapper;
    [[no_unique_address]] FunctionWrapper<104, typestring_is("onTriggerExit")> onTriggerExitMethodWrapper;
    [[no_unique_address]] FunctionWrapper<105, typestring_is("onUpdate")> onUpdateMethodWrapper;
    [[no_unique_address]] FunctionWrapper<106, typestring_is("onDestroy")> onDestroyMethodWrapper;
    [[no_unique_address]] FunctionWrapper<107, typestring_is("onEnable")> onEnableMethodWrapper;
    [[no_unique_address]] FunctionWrapper<108, typestring_is("onDisable")> onDisableMethodWrapper;
    [[no_unique_address]] FunctionWrapper<109, typestring_is("onButtonPress")> onButtonPressMethodWrapper;
    [[no_unique_address]] FunctionWrapper<110, typestring_is("onButtonRelease")> onButtonReleaseMethodWrapper;
    // Agent state-machine event wrappers (IDs 200-206)
    [[no_unique_address]] FunctionWrapper<200, typestring_is("onStateEnter")>    onStateEnterMethodWrapper;
    [[no_unique_address]] FunctionWrapper<201, typestring_is("onStateExit")>     onStateExitMethodWrapper;
    [[no_unique_address]] FunctionWrapper<202, typestring_is("onTargetSeen")>    onTargetSeenMethodWrapper;
    [[no_unique_address]] FunctionWrapper<203, typestring_is("onTargetLost")>    onTargetLostMethodWrapper;
    [[no_unique_address]] FunctionWrapper<204, typestring_is("onTargetReached")> onTargetReachedMethodWrapper;
    [[no_unique_address]] FunctionWrapper<205, typestring_is("onPatrolPoint")>   onPatrolPointMethodWrapper;
    [[no_unique_address]] FunctionWrapper<206, typestring_is("onPathBlocked")>   onPathBlockedMethodWrapper;
    
    void PushGameObject(GameObject* go);

  private:
    psyqo::Lua m_state;

    int m_metatableReference = LUA_NOREF;
    int m_luascriptsReference = LUA_NOREF;
    int m_luaSceneScriptsReference = LUA_NOREF;

    // Bytecode references for per-object re-execution.
    // Points into splashpack data which stays in memory for the scene lifetime.
    static constexpr int MAX_LUA_FILES = 32;
    struct BytecodeRef {
        const char* code;
        size_t len;
    };
    BytecodeRef m_bytecodeRefs[MAX_LUA_FILES];
    int m_bytecodeRefCount = 0;

    template <int methodId, typename methodName>
    friend struct FunctionWrapper;
};
}  // namespace psxsplash
