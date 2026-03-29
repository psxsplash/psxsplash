#include "lua.h"

#include <psyqo-lua/lua.hh>

#include <psyqo/alloc.h>
#include <psyqo/soft-math.hh>
#include <psyqo/trigonometry.hh>
#include <psyqo/xprintf.h>

#include "gameobject.hh"

// OOM-guarded allocator for Lua. The linker redirects luaI_realloc
// here instead of straight to psyqo_realloc, so we can log before
// returning NULL.
extern "C" void *lua_oom_realloc(void *ptr, size_t size) {
    void *result = psyqo_realloc(ptr, size);
    if (!result && size > 0) {
        printf("Lua OOM: alloc %u bytes failed\n", (unsigned)size);
    }
    return result;
}

// Pre-compiled PS1 Lua bytecode for the GameObject metatable script.
// Compiled with luac_psx to avoid needing the Lua parser at runtime.
#include "gameobject_bytecode.h"

// Lua helpers

static constexpr int32_t kFixedScale = 4096;

// Accept FixedPoint object or plain number from Lua
static psyqo::FixedPoint<12> readFP(psyqo::Lua& L, int idx) {
    if (L.isFixedPoint(idx)) return L.toFixedPoint(idx);
    return psyqo::FixedPoint<12>(static_cast<int32_t>(L.toNumber(idx) * kFixedScale), psyqo::FixedPoint<12>::RAW);
}

static int gameobjectGetPosition(psyqo::Lua L) {

    auto go = L.toUserdata<psxsplash::GameObject>(1);

    L.newTable();
    L.push(go->position.x);
    L.setField(2, "x");
    L.push(go->position.y);
    L.setField(2, "y");
    L.push(go->position.z);
    L.setField(2, "z");

    return 1;

}

static int gameobjectSetPosition(psyqo::Lua L) {

    auto go = L.toUserdata<psxsplash::GameObject>(1);

    L.getField(2, "x");
    go->position.x = readFP(L, 3);
    L.pop();

    L.getField(2, "y");
    go->position.y = readFP(L, 3);
    L.pop();

    L.getField(2, "z");
    go->position.z = readFP(L, 3);
    L.pop();
    return 0;

}

static int gameobjectGetActive(psyqo::Lua L) {
    auto go = L.toUserdata<psxsplash::GameObject>(1);
    L.push(go->isActive());
    return 1;
}

static int gameobjectSetActive(psyqo::Lua L) {
    auto go = L.toUserdata<psxsplash::GameObject>(1);
    bool active = L.toBoolean(2);
    go->setActive(active);
    return 0;
}

static psyqo::Trig<> s_trig;

static psyqo::Angle fastAtan2(int32_t sinVal, int32_t cosVal) {
    psyqo::Angle result;
    if (cosVal == 0 && sinVal == 0) { result.value = 0; return result; }

    int32_t abs_s = sinVal < 0 ? -sinVal : sinVal;
    int32_t abs_c = cosVal < 0 ? -cosVal : cosVal;

    int32_t minV = abs_s < abs_c ? abs_s : abs_c;
    int32_t maxV = abs_s > abs_c ? abs_s : abs_c;
    int32_t angle = (minV * 256) / maxV;

    if (abs_s > abs_c) angle = 512 - angle;
    if (cosVal < 0) angle = 1024 - angle;
    if (sinVal < 0) angle = -angle;

    result.value = angle;
    return result;
}

static int gameobjectGetRotation(psyqo::Lua L) {
    auto go = L.toUserdata<psxsplash::GameObject>(1);
    // Decompose Y-axis rotation from the matrix (vs[0].x = cos, vs[0].z = sin)
    // For full XYZ, we extract approximate Euler angles from the rotation matrix.
    // Row 0: [cos(Y)cos(Z), -cos(Y)sin(Z), sin(Y)]
    // This is a simplified extraction assuming common rotation order Y*X*Z.
    int32_t sinY = go->rotation.vs[0].z.raw();
    int32_t cosY = go->rotation.vs[0].x.raw();
    int32_t sinX = -go->rotation.vs[1].z.raw();
    int32_t cosX = go->rotation.vs[2].z.raw();
    int32_t sinZ = -go->rotation.vs[0].y.raw();
    int32_t cosZ = go->rotation.vs[0].x.raw();

    auto toFP12 = [](psyqo::Angle a) -> psyqo::FixedPoint<12> {
        psyqo::FixedPoint<12> fp;
        fp.value = a.value << 2;
        return fp;
    };

    L.newTable();
    L.push(toFP12(fastAtan2(sinX, cosX)));
    L.setField(2, "x");
    L.push(toFP12(fastAtan2(sinY, cosY)));
    L.setField(2, "y");
    L.push(toFP12(fastAtan2(sinZ, cosZ)));
    L.setField(2, "z");

    return 1;
}

static int gameobjectSetRotation(psyqo::Lua L) {
    auto go = L.toUserdata<psxsplash::GameObject>(1);

    L.getField(2, "x");
    psyqo::FixedPoint<12> fpX = readFP(L, 3);
    L.pop();

    L.getField(2, "y");
    psyqo::FixedPoint<12> fpY = readFP(L, 3);
    L.pop();

    L.getField(2, "z");
    psyqo::FixedPoint<12> fpZ = readFP(L, 3);
    L.pop();

    // Convert FixedPoint<12> to Angle (FixedPoint<10>)
    psyqo::Angle rx, ry, rz;
    rx.value = fpX.value >> 2;
    ry.value = fpY.value >> 2;
    rz.value = fpZ.value >> 2;

    // Compose Y * X * Z rotation matrix
    auto matY = psyqo::SoftMath::generateRotationMatrix33(ry, psyqo::SoftMath::Axis::Y, s_trig);
    auto matX = psyqo::SoftMath::generateRotationMatrix33(rx, psyqo::SoftMath::Axis::X, s_trig);
    auto matZ = psyqo::SoftMath::generateRotationMatrix33(rz, psyqo::SoftMath::Axis::Z, s_trig);
    auto temp = psyqo::SoftMath::multiplyMatrix33(matY, matX);
    go->rotation = psyqo::SoftMath::multiplyMatrix33(temp, matZ);
    return 0;
}


static int gameobjectGetRotationY(psyqo::Lua L) {
    auto go = L.toUserdata<psxsplash::GameObject>(1);
    int32_t sinRaw = go->rotation.vs[0].z.raw();
    int32_t cosRaw = go->rotation.vs[0].x.raw();
    psyqo::Angle angle = fastAtan2(sinRaw, cosRaw);
    // Angle is FixedPoint<10> (pi-units). Convert to FixedPoint<12> for Lua.
    psyqo::FixedPoint<12> fp12;
    fp12.value = angle.value << 2;
    L.push(fp12);
    return 1;
}

static int gameobjectSetRotationY(psyqo::Lua L) {
    auto go = L.toUserdata<psxsplash::GameObject>(1);
    // Accept FixedPoint<12> from Lua, convert to Angle (FixedPoint<10>)
    psyqo::FixedPoint<12> fp12 = readFP(L, 2);
    psyqo::Angle angle;
    angle.value = fp12.value >> 2;
    go->rotation = psyqo::SoftMath::generateRotationMatrix33(angle, psyqo::SoftMath::Axis::Y, s_trig);
    return 0;
}

void psxsplash::Lua::Init() {
    auto L = m_state;
    // Load and run the game objects script
    if (L.loadBuffer(reinterpret_cast<const char*>(GAMEOBJECT_BYTECODE), sizeof(GAMEOBJECT_BYTECODE), "bytecode:gameObjects") == 0) {
        if (L.pcall(0, 1) == 0) {
            // This will be our metatable
            L.newTable();

            L.push(gameobjectGetPosition);
            L.setField(-2, "get_position");

            L.push(gameobjectSetPosition);
            L.setField(-2, "set_position");

            L.push(gameobjectGetActive);
            L.setField(-2, "get_active");

            L.push(gameobjectSetActive);
            L.setField(-2, "set_active");

            L.push(gameobjectGetRotation);
            L.setField(-2, "get_rotation");

            L.push(gameobjectSetRotation);
            L.setField(-2, "set_rotation");

            L.push(gameobjectGetRotationY);
            L.setField(-2, "get_rotationY");

            L.push(gameobjectSetRotationY);
            L.setField(-2, "set_rotationY");

            L.copy(-1);
            m_metatableReference = L.ref();

            if (L.pcall(1, 0) == 0) {
                // success
            } else {
                printf("Error registering Lua script: %s\n", L.optString(-1, "Unknown error"));
                L.clearStack();
                return;
            }
        } else {
            // Print Lua error if script execution fails
            printf("Error executing Lua script: %s\n", L.optString(-1, "Unknown error"));
            L.clearStack();
            return;
        }
    } else {
        // Print Lua error if script loading fails
        printf("Error loading Lua script: %s\n", L.optString(-1, "Unknown error"));
        L.clearStack();
        return;
    }

    L.newTable();
    m_luascriptsReference = L.ref();

    // Add __concat to the FixedPoint metatable so FixedPoint values work with ..
    // psyqo-lua doesn't provide this, but scripts need it for Debug.Log etc.
    L.getField(LUA_REGISTRYINDEX, "psyqo.FixedPoint");
    if (L.isTable(-1)) {
        L.push([](psyqo::Lua L) -> int {
            // Convert both operands to strings and concatenate
            char buf[64];
            int len = 0;

            for (int i = 1; i <= 2; i++) {
                if (L.isFixedPoint(i)) {
                    auto fp = L.toFixedPoint(i);
                    int32_t raw = fp.raw();
                    int integer = raw >> 12;
                    unsigned fraction = (raw < 0 ? -raw : raw) & 0xfff;
                    if (fraction == 0) {
                        len += snprintf(buf + len, sizeof(buf) - len, "%d", integer);
                    } else {
                        unsigned decimal = (fraction * 1000) >> 12;
                        if (raw < 0 && integer == 0)
                            len += snprintf(buf + len, sizeof(buf) - len, "-%d.%03u", integer, decimal);
                        else
                            len += snprintf(buf + len, sizeof(buf) - len, "%d.%03u", integer, decimal);
                    }
                } else {
                    const char* s = L.toString(i);
                    if (s) {
                        int slen = 0;
                        while (s[slen]) slen++;
                        if (len + slen < (int)sizeof(buf)) {
                            for (int j = 0; j < slen; j++) buf[len++] = s[j];
                        }
                    }
                }
            }
            buf[len] = '\0';
            L.push(buf, len);
            return 1;
        });
        L.setField(-2, "__concat");
    }
    L.pop();
}

void psxsplash::Lua::Shutdown() {

    if (m_state.getState()) {
        m_state.close();
    }
    m_metatableReference = LUA_NOREF;
    m_luascriptsReference = LUA_NOREF;
    m_luaSceneScriptsReference = LUA_NOREF;
}

void psxsplash::Lua::Reset() {
    Shutdown();
    m_state = psyqo::Lua();
    Init();
}

void psxsplash::Lua::LoadLuaFile(const char* code, size_t len, int index) {
    // Store bytecode reference for per-object re-execution in RegisterGameObject.
    if (index < MAX_LUA_FILES) {
        m_bytecodeRefs[index] = {code, len};
        if (index >= m_bytecodeRefCount) m_bytecodeRefCount = index + 1;
    }

    auto L = m_state;
    char filename[32];
    snprintf(filename, sizeof(filename), "lua_asset:%d", index);
    if (L.loadBuffer(code, len, filename) != LUA_OK) {
        printf("Lua error: %s\n", L.toString(-1));
        L.pop();
        return;
    }
    // (1) script func
    L.rawGetI(LUA_REGISTRYINDEX, m_luascriptsReference);
    // (1) script func (2) scripts table
    L.newTable();
    // (1) script func (2) scripts table (3) env {}

    // Give the environment a metatable that falls back to _G
    // so scripts can see Entity, Debug, Input, etc.
    L.newTable();
    // (1) script func (2) scripts table (3) env {} (4) mt {}
    L.pushGlobalTable();
    // (1) script func (2) scripts table (3) env {} (4) mt {} (5) _G
    L.setField(-2, "__index");
    // (1) script func (2) scripts table (3) env {} (4) mt { __index = _G }
    L.setMetatable(-2);
    // (1) script func (2) scripts table (3) env { mt }

    L.pushNumber(index);
    // (1) script func (2) scripts table (3) env (4) index
    L.copy(-2);
    // (1) script func (2) scripts table (3) env (4) index (5) env
    L.setTable(-4);
    // (1) script func (2) scripts table (3) env
    lua_setupvalue(L.getState(), -3, 1);
    // (1) script func (2) scripts table
    L.pop();
    // (1) script func
    if (L.pcall(0, 0)) {
        printf("Lua error: %s\n", L.toString(-1));
        L.pop();
    }
}

void psxsplash::Lua::RegisterSceneScripts(int index) {
    if (index < 0) return;
    auto L = m_state;
    L.newTable();
    // (1) {}
    L.copy(1);
    // (1) {} (2) {}
    m_luaSceneScriptsReference = L.ref();
    // (1) {}
    L.rawGetI(LUA_REGISTRYINDEX, m_luascriptsReference);
    // (1) {} (2) scripts table
    L.pushNumber(index);
    // (1) {} (2) script environments table (3) index
    L.getTable(-2);
    // (1) {} (2) script environments table (3) script environment table for the scene
    if (!L.isTable(-1)) {
        // Scene Lua file index is invalid or script not loaded
        printf("Warning: scene Lua file index %d not found\n", index);
        L.pop(3);
        return;
    }
    onSceneCreationStartFunctionWrapper.resolveGlobal(L);
    onSceneCreationEndFunctionWrapper.resolveGlobal(L);
    L.pop(3);
    // empty stack
}

void psxsplash::Lua::RegisterGameObject(GameObject* go) {
    uint8_t* ptr = reinterpret_cast<uint8_t*>(go);
    auto L = m_state;
    L.push(ptr);
    // (1) go
    L.newTable();
    // (1) go (2) {}
    L.push(ptr);
    // (1) go (2) {} (3) go
    L.setField(-2, "__cpp_ptr");
    // (1) go (2) { __cpp_ptr = go }
    L.rawGetI(LUA_REGISTRYINDEX, m_metatableReference);
    // (1) go (2) { __cpp_ptr = go } (3) metatable
    if (L.isTable(-1)) {
        L.setMetatable(-2);
    } else {
        printf("Warning: metatableForAllGameObjects not found\n");
        L.pop();
    }
    // (1) go (2) { __cpp_ptr = go + metatable }
    L.rawSet(LUA_REGISTRYINDEX);
    // empty stack
    L.newTable();
    // (1) {}
    L.push(ptr + 1);
    // (1) {} (2) go + 1
    L.copy(1);
    // (1) {} (2) go + 1 (3) {}
    L.rawSet(LUA_REGISTRYINDEX);
    // (1) {}
    
    // Initialize event mask for this object
    uint32_t eventMask = EVENT_NONE;

    if (go->luaFileIndex != -1 && go->luaFileIndex < m_bytecodeRefCount) {
        auto& ref = m_bytecodeRefs[go->luaFileIndex];
        char filename[32];
        snprintf(filename, sizeof(filename), "lua_asset:%d", go->luaFileIndex);

        if (L.loadBuffer(ref.code, ref.len, filename) == LUA_OK) {
            // (1) method_table (2) chunk_func

            // Create a per-object environment with __index = _G
            // so this object's file-level locals are isolated.
            L.newTable();
            L.newTable();
            L.pushGlobalTable();
            L.setField(-2, "__index");
            L.setMetatable(-2);
            // (1) method_table (2) chunk_func (3) env

            // Set env as the chunk's _ENV upvalue
            L.copy(-1);
            // (1) method_table (2) chunk_func (3) env (4) env_copy
            lua_setupvalue(L.getState(), -3, 1);
            // (1) method_table (2) chunk_func (3) env

            // Move chunk to top for pcall
            lua_insert(L.getState(), -2);
            // (1) method_table (2) env (3) chunk_func

            if (L.pcall(0, 0) == LUA_OK) {
                // (1) method_table (2) env
                // resolveGlobal expects: (1) method_table, (3) env
                // Insert a placeholder at position 2 to push env to position 3
                L.push();  // push nil
                // (1) method_table (2) env (3) nil
                lua_insert(L.getState(), 2);
                // (1) method_table (2) nil (3) env

                // Resolve each event - creates fresh function refs with isolated upvalues
                if (onCreateMethodWrapper.resolveGlobal(L))              eventMask |= EVENT_ON_CREATE;
                if (onCollideWithPlayerMethodWrapper.resolveGlobal(L))   eventMask |= EVENT_ON_COLLISION;
                if (onInteractMethodWrapper.resolveGlobal(L))            eventMask |= EVENT_ON_INTERACT;
                if (onTriggerEnterMethodWrapper.resolveGlobal(L))        eventMask |= EVENT_ON_TRIGGER_ENTER;
                if (onTriggerExitMethodWrapper.resolveGlobal(L))         eventMask |= EVENT_ON_TRIGGER_EXIT;
                if (onUpdateMethodWrapper.resolveGlobal(L))              eventMask |= EVENT_ON_UPDATE;
                if (onDestroyMethodWrapper.resolveGlobal(L))             eventMask |= EVENT_ON_DESTROY;
                if (onEnableMethodWrapper.resolveGlobal(L))              eventMask |= EVENT_ON_ENABLE;
                if (onDisableMethodWrapper.resolveGlobal(L))             eventMask |= EVENT_ON_DISABLE;
                if (onButtonPressMethodWrapper.resolveGlobal(L))         eventMask |= EVENT_ON_BUTTON_PRESS;
                if (onButtonReleaseMethodWrapper.resolveGlobal(L))       eventMask |= EVENT_ON_BUTTON_RELEASE;

                L.pop(2); // pop nil and env
            } else {
                printf("Lua error: %s\n", L.toString(-1));
                L.pop(2); // pop error msg and env
            }
        } else {
            printf("Lua error: %s\n", L.toString(-1));
            L.pop(); // pop error msg
        }
    }
    
    // Store the event mask directly in the GameObject
    go->eventMask = eventMask;

    L.pop();
    // empty stack
    // Note: onCreate is NOT fired here. Call FireAllOnCreate() after all objects
    // are registered so that Entity.Find works across all objects in onCreate.
}

void psxsplash::Lua::FireAllOnCreate(GameObject** objects, size_t count) {
    for (size_t i = 0; i < count; i++) {
        if (objects[i] && (objects[i]->eventMask & EVENT_ON_CREATE)) {
            onCreateMethodWrapper.callMethod(*this, objects[i]);
        }
    }
}

void psxsplash::Lua::OnCollideWithPlayer(GameObject* self) {
    if (!hasEvent(self, EVENT_ON_COLLISION)) return;
    onCollideWithPlayerMethodWrapper.callMethod(*this, self);
}

void psxsplash::Lua::OnInteract(GameObject* self) {
    if (!hasEvent(self, EVENT_ON_INTERACT)) return;
    onInteractMethodWrapper.callMethod(*this, self);
}

void psxsplash::Lua::OnTriggerEnter(GameObject* trigger, GameObject* other) {
    if (!hasEvent(trigger, EVENT_ON_TRIGGER_ENTER)) return;
    onTriggerEnterMethodWrapper.callMethod(*this, trigger, other);
}

void psxsplash::Lua::OnTriggerExit(GameObject* trigger, GameObject* other) {
    if (!hasEvent(trigger, EVENT_ON_TRIGGER_EXIT)) return;
    onTriggerExitMethodWrapper.callMethod(*this, trigger, other);
}

void psxsplash::Lua::OnTriggerEnterScript(int luaFileIndex, int triggerIndex) {
    auto L = m_state;
    L.rawGetI(LUA_REGISTRYINDEX, m_luascriptsReference);
    L.rawGetI(-1, luaFileIndex);
    if (!L.isTable(-1)) { L.clearStack(); return; }
    L.push("onTriggerEnter", 14);
    L.getTable(-2);
    if (!L.isFunction(-1)) { L.clearStack(); return; }
    L.pushNumber(triggerIndex);
    if (L.pcall(1, 0) != LUA_OK) {
        printf("Lua error: %s\n", L.toString(-1));
    }
    L.clearStack();
}

void psxsplash::Lua::OnTriggerExitScript(int luaFileIndex, int triggerIndex) {
    auto L = m_state;
    L.rawGetI(LUA_REGISTRYINDEX, m_luascriptsReference);
    L.rawGetI(-1, luaFileIndex);
    if (!L.isTable(-1)) { L.clearStack(); return; }
    L.push("onTriggerExit", 13);
    L.getTable(-2);
    if (!L.isFunction(-1)) { L.clearStack(); return; }
    L.pushNumber(triggerIndex);
    if (L.pcall(1, 0) != LUA_OK) {
        printf("Lua error: %s\n", L.toString(-1));
    }
    L.clearStack();
}

void psxsplash::Lua::OnDestroy(GameObject* go) {
    if (!hasEvent(go, EVENT_ON_DESTROY)) return;
    onDestroyMethodWrapper.callMethod(*this, go);
    go->eventMask = EVENT_NONE;
}

void psxsplash::Lua::OnEnable(GameObject* go) {
    if (!hasEvent(go, EVENT_ON_ENABLE)) return;
    onEnableMethodWrapper.callMethod(*this, go);
}

void psxsplash::Lua::OnDisable(GameObject* go) {
    if (!hasEvent(go, EVENT_ON_DISABLE)) return;
    onDisableMethodWrapper.callMethod(*this, go);
}

void psxsplash::Lua::OnButtonPress(GameObject* go, int button) {
    if (!hasEvent(go, EVENT_ON_BUTTON_PRESS)) return;
    onButtonPressMethodWrapper.callMethod(*this, go, button);
}

void psxsplash::Lua::OnButtonRelease(GameObject* go, int button) {
    if (!hasEvent(go, EVENT_ON_BUTTON_RELEASE)) return;
    onButtonReleaseMethodWrapper.callMethod(*this, go, button);
}

void psxsplash::Lua::OnUpdate(GameObject* go, int deltaFrames) {
    if (!hasEvent(go, EVENT_ON_UPDATE)) return;
    onUpdateMethodWrapper.callMethod(*this, go, deltaFrames);
}

void psxsplash::Lua::RelocateGameObjects(GameObject** objects, size_t count, intptr_t delta) {
    auto L = m_state;
    for (size_t i = 0; i < count; i++) {
        uint8_t* newPtr = reinterpret_cast<uint8_t*>(objects[i]);
        uint8_t* oldPtr = newPtr - delta;

        // Re-key the main game object table: registry[oldPtr] -> registry[newPtr]
        L.push(oldPtr);
        L.rawGet(LUA_REGISTRYINDEX);
        if (L.isTable(-1)) {
            // Update __cpp_ptr inside the table
            L.push(newPtr);
            L.setField(-2, "__cpp_ptr");
            // Store at new key
            L.push(newPtr);
            L.copy(-2);
            L.rawSet(LUA_REGISTRYINDEX);
            // Remove old key
            L.push(oldPtr);
            L.push();  // nil
            L.rawSet(LUA_REGISTRYINDEX);
        }
        L.pop();

        // Re-key the methods table: registry[oldPtr+1] -> registry[newPtr+1]
        L.push(oldPtr + 1);
        L.rawGet(LUA_REGISTRYINDEX);
        if (L.isTable(-1)) {
            L.push(newPtr + 1);
            L.copy(-2);
            L.rawSet(LUA_REGISTRYINDEX);
            L.push(oldPtr + 1);
            L.push();
            L.rawSet(LUA_REGISTRYINDEX);
        }
        L.pop();
    }
}

void psxsplash::Lua::PushGameObject(GameObject* go) {
    auto L = m_state;
    L.push(go);
    L.rawGet(LUA_REGISTRYINDEX);

    if (!L.isTable(-1)) {
        L.pop();
        L.push(); // push nil so the caller always gets a value
    }
}
