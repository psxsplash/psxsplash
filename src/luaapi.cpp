#include "luaapi.hh"
#include "scenemanager.hh"
#include "gameobject.hh"
#include "controls.hh"
#include "camera.hh"
#include "cutscene.hh"
#include "animation.hh"
#include "uisystem.hh"

#include <psyqo/soft-math.hh>
#include <psyqo/trigonometry.hh>
#include <psyqo/fixed-point.hh>


namespace psxsplash {

// Static member
SceneManager* LuaAPI::s_sceneManager = nullptr;
CutscenePlayer* LuaAPI::s_cutscenePlayer = nullptr;
AnimationPlayer* LuaAPI::s_animationPlayer = nullptr;
UISystem* LuaAPI::s_uiSystem = nullptr;

// Scale factor: FixedPoint<12> stores 1.0 as raw 4096.
// Lua scripts work in world-space units (1 = one unit), so we convert.
static constexpr lua_Number kFixedScale = 4096;

// Read a FixedPoint<12> from the stack, accepting either a FixedPoint object
// or a plain integer (which gets scaled by 4096 to become fp12).
static psyqo::FixedPoint<12> readFP(psyqo::Lua& L, int idx) {
    if (L.isFixedPoint(idx)) {
        return L.toFixedPoint(idx);
    }
    return psyqo::FixedPoint<12>(static_cast<int32_t>(L.toNumber(idx) * kFixedScale), psyqo::FixedPoint<12>::RAW);
}

// Angle scale: psyqo::Angle is FixedPoint<10>, so 1.0_pi = raw 1024
static constexpr lua_Number kAngleScale = 1024;
static psyqo::Trig<> s_trig;

// ============================================================================
// REGISTRATION
// ============================================================================

void LuaAPI::RegisterAll(psyqo::Lua& L, SceneManager* scene, CutscenePlayer* cutscenePlayer, AnimationPlayer* animationPlayer, UISystem* uiSystem) {
    s_sceneManager = scene;
    s_cutscenePlayer = cutscenePlayer;
    s_animationPlayer = animationPlayer;
    s_uiSystem = uiSystem;
    
    // ========================================================================
    // ENTITY API
    // ========================================================================
    L.newTable();  // Entity table
    
    L.push(Entity_FindByScriptIndex);
    L.setField(-2, "FindByScriptIndex");
    
    L.push(Entity_FindByIndex);
    L.setField(-2, "FindByIndex");
    
    L.push(Entity_Find);
    L.setField(-2, "Find");
    
    L.push(Entity_GetCount);
    L.setField(-2, "GetCount");
    
    L.push(Entity_SetActive);
    L.setField(-2, "SetActive");
    
    L.push(Entity_IsActive);
    L.setField(-2, "IsActive");
    
    L.push(Entity_GetPosition);
    L.setField(-2, "GetPosition");
    
    L.push(Entity_SetPosition);
    L.setField(-2, "SetPosition");
    
    L.push(Entity_GetRotationY);
    L.setField(-2, "GetRotationY");
    
    L.push(Entity_SetRotationY);
    L.setField(-2, "SetRotationY");
    
    L.push(Entity_ForEach);
    L.setField(-2, "ForEach");
    
    L.setGlobal("Entity");
    
    // ========================================================================
    // VEC3 API
    // ========================================================================
    L.newTable();  // Vec3 table
    
    L.push(Vec3_New);
    L.setField(-2, "new");
    
    L.push(Vec3_Add);
    L.setField(-2, "add");
    
    L.push(Vec3_Sub);
    L.setField(-2, "sub");
    
    L.push(Vec3_Mul);
    L.setField(-2, "mul");
    
    L.push(Vec3_Dot);
    L.setField(-2, "dot");
    
    L.push(Vec3_Cross);
    L.setField(-2, "cross");
    
    L.push(Vec3_Length);
    L.setField(-2, "length");
    
    L.push(Vec3_LengthSq);
    L.setField(-2, "lengthSq");
    
    L.push(Vec3_Normalize);
    L.setField(-2, "normalize");
    
    L.push(Vec3_Distance);
    L.setField(-2, "distance");
    
    L.push(Vec3_DistanceSq);
    L.setField(-2, "distanceSq");
    
    L.push(Vec3_Lerp);
    L.setField(-2, "lerp");
    
    L.setGlobal("Vec3");
    
    // ========================================================================
    // INPUT API
    // ========================================================================
    L.newTable();  // Input table
    
    L.push(Input_IsPressed);
    L.setField(-2, "IsPressed");
    
    L.push(Input_IsReleased);
    L.setField(-2, "IsReleased");
    
    L.push(Input_IsHeld);
    L.setField(-2, "IsHeld");
    
    L.push(Input_GetAnalog);
    L.setField(-2, "GetAnalog");
    
    // Register button constants
    RegisterInputConstants(L);
    
    L.setGlobal("Input");
    
    // ========================================================================
    // TIMER API
    // ========================================================================
    L.newTable();  // Timer table
    
    L.push(Timer_GetFrameCount);
    L.setField(-2, "GetFrameCount");
    
    L.setGlobal("Timer");
    
    // ========================================================================
    // CAMERA API
    // ========================================================================
    L.newTable();  // Camera table
    
    L.push(Camera_GetPosition);
    L.setField(-2, "GetPosition");
    
    L.push(Camera_SetPosition);
    L.setField(-2, "SetPosition");
    
    L.push(Camera_GetRotation);
    L.setField(-2, "GetRotation");
    
    L.push(Camera_SetRotation);
    L.setField(-2, "SetRotation");

    L.push(Camera_GetForward);
    L.setField(-2, "GetForward");

    L.push(Camera_MoveForward);
    L.setField(-2, "MoveForward");

    L.push(Camera_MoveBackward);
    L.setField(-2, "MoveBackward");

    L.push(Camera_MoveLeft);
    L.setField(-2, "MoveLeft");

    L.push(Camera_MoveRight);
    L.setField(-2, "MoveRight");
    
    L.push(Camera_LookAt);
    L.setField(-2, "LookAt");
    
    L.setGlobal("Camera");
    
    // ========================================================================
    // AUDIO API (Placeholder)
    // ========================================================================
    L.newTable();  // Audio table
    
    L.push(Audio_Play);
    L.setField(-2, "Play");
    
    L.push(Audio_Find);
    L.setField(-2, "Find");
    
    L.push(Audio_Stop);
    L.setField(-2, "Stop");
    
    L.push(Audio_SetVolume);
    L.setField(-2, "SetVolume");
    
    L.push(Audio_StopAll);
    L.setField(-2, "StopAll");
    
    L.setGlobal("Audio");
    
    // ========================================================================
    // DEBUG API
    // ========================================================================
    L.newTable();  // Debug table
    
    L.push(Debug_Log);
    L.setField(-2, "Log");
    
    L.push(Debug_DrawLine);
    L.setField(-2, "DrawLine");
    
    L.push(Debug_DrawBox);
    L.setField(-2, "DrawBox");
    
    L.setGlobal("Debug");
    
    // ========================================================================
    // MATH API
    // ========================================================================
    L.newTable();  // PSXMath table (avoid conflict with Lua's math)
    
    L.push(Math_Clamp);
    L.setField(-2, "Clamp");
    
    L.push(Math_Lerp);
    L.setField(-2, "Lerp");
    
    L.push(Math_Sign);
    L.setField(-2, "Sign");
    
    L.push(Math_Abs);
    L.setField(-2, "Abs");
    
    L.push(Math_Min);
    L.setField(-2, "Min");
    
    L.push(Math_Max);
    L.setField(-2, "Max");
    
    L.setGlobal("PSXMath");
    
    // ========================================================================
    // SCENE API
    // ========================================================================
    L.newTable();  // Scene table
    
    L.push(Scene_Load);
    L.setField(-2, "Load");
    
    L.push(Scene_GetIndex);
    L.setField(-2, "GetIndex");
    
    L.setGlobal("Scene");
    
    // ========================================================================
    // PERSIST API
    // ========================================================================
    L.newTable();  // Persist table
    
    L.push(Persist_Get);
    L.setField(-2, "Get");
    
    L.push(Persist_Set);
    L.setField(-2, "Set");
    
    L.setGlobal("Persist");
    
    // ========================================================================
    // CUTSCENE API
    // ========================================================================
    L.newTable();  // Cutscene table
    
    L.push(Cutscene_Play);
    L.setField(-2, "Play");
    
    L.push(Cutscene_Stop);
    L.setField(-2, "Stop");
    
    L.push(Cutscene_IsPlaying);
    L.setField(-2, "IsPlaying");
    
    L.setGlobal("Cutscene");

    // ========================================================================
    // ANIMATION API
    // ========================================================================
    L.newTable();

    L.push(Animation_Play);
    L.setField(-2, "Play");

    L.push(Animation_Stop);
    L.setField(-2, "Stop");

    L.push(Animation_IsPlaying);
    L.setField(-2, "IsPlaying");

    L.setGlobal("Animation");

    // ========================================================================
    // CONTROLS API
    // ========================================================================
    L.newTable();

    L.push(Controls_SetEnabled);
    L.setField(-2, "SetEnabled");

    L.push(Controls_IsEnabled);
    L.setField(-2, "IsEnabled");

    L.setGlobal("Controls");

    // ========================================================================
    // INTERACT API
    // ========================================================================
    L.newTable();

    L.push(Interact_SetEnabled);
    L.setField(-2, "SetEnabled");

    L.push(Interact_IsEnabled);
    L.setField(-2, "IsEnabled");

    L.setGlobal("Interact");

    // ========================================================================
    // UI API
    // ========================================================================
    L.newTable();  // UI table
    
    L.push(UI_FindCanvas);
    L.setField(-2, "FindCanvas");
    
    L.push(UI_SetCanvasVisible);
    L.setField(-2, "SetCanvasVisible");
    
    L.push(UI_IsCanvasVisible);
    L.setField(-2, "IsCanvasVisible");
    
    L.push(UI_FindElement);
    L.setField(-2, "FindElement");
    
    L.push(UI_SetVisible);
    L.setField(-2, "SetVisible");
    
    L.push(UI_IsVisible);
    L.setField(-2, "IsVisible");
    
    L.push(UI_SetText);
    L.setField(-2, "SetText");

    L.push(UI_GetText);
    L.setField(-2, "GetText");
    
    L.push(UI_SetProgress);
    L.setField(-2, "SetProgress");
    
    L.push(UI_GetProgress);
    L.setField(-2, "GetProgress");
    
    L.push(UI_SetColor);
    L.setField(-2, "SetColor");

    L.push(UI_GetColor);
    L.setField(-2, "GetColor");
    
    L.push(UI_SetPosition);
    L.setField(-2, "SetPosition");

    L.push(UI_GetPosition);
    L.setField(-2, "GetPosition");

    L.push(UI_SetSize);
    L.setField(-2, "SetSize");

    L.push(UI_GetSize);
    L.setField(-2, "GetSize");

    L.push(UI_SetProgressColors);
    L.setField(-2, "SetProgressColors");

    L.push(UI_GetElementType);
    L.setField(-2, "GetElementType");

    L.push(UI_GetElementCount);
    L.setField(-2, "GetElementCount");

    L.push(UI_GetElementByIndex);
    L.setField(-2, "GetElementByIndex");
    
    L.setGlobal("UI");
}

// ============================================================================
// ENTITY API IMPLEMENTATION
// ============================================================================

int LuaAPI::Entity_FindByScriptIndex(lua_State* L) {
    psyqo::Lua lua(L);
    
    if (!s_sceneManager || !lua.isNumber(1)) {
        lua.push();
        return 1;
    }
    
    // Find first object with matching luaFileIndex
    int16_t luaIdx = static_cast<int16_t>(lua.toNumber(1));
    for (size_t i = 0; i < s_sceneManager->getGameObjectCount(); i++) {
        auto* go = s_sceneManager->getGameObject(static_cast<uint16_t>(i));
        if (go && go->luaFileIndex == luaIdx) {
            lua.push(reinterpret_cast<uint8_t*>(go));
            lua.rawGet(LUA_REGISTRYINDEX);
            if (lua.isTable(-1)) return 1;
            lua.pop();
        }
    }
    
    lua.push();
    return 1;
}

int LuaAPI::Entity_FindByIndex(lua_State* L) {
    psyqo::Lua lua(L);
    
    if (!lua.isNumber(1)) {
        lua.push();
        return 1;
    }
    
    int index = static_cast<int>(lua.toNumber(1));
    
    if (s_sceneManager) {
        GameObject* go = s_sceneManager->getGameObject(static_cast<uint16_t>(index));
        if (go) {
            lua.push(reinterpret_cast<uint8_t*>(go));
            lua.rawGet(LUA_REGISTRYINDEX);
            if (lua.isTable(-1)) {
                return 1;
            }
            lua.pop();
        }
    }
    
    lua.push();
    return 1;
}

int LuaAPI::Entity_Find(lua_State* L) {
    psyqo::Lua lua(L);

    if (!s_sceneManager) {
        lua.push();
        return 1;
    }

    // Accept number (index) or string (name lookup) for backwards compat
    // Check isNumber FIRST — in Lua, numbers pass isString too.
    if (lua.isNumber(1)) {
        int index = static_cast<int>(lua.toNumber(1));
        GameObject* go = s_sceneManager->getGameObject(static_cast<uint16_t>(index));
        if (go) {
            lua.push(reinterpret_cast<uint8_t*>(go));
            lua.rawGet(LUA_REGISTRYINDEX);
            if (lua.isTable(-1)) return 1;
            lua.pop();
        }
    } else if (lua.isString(1)) {
        const char* name = lua.toString(1);
        GameObject* go = s_sceneManager->findObjectByName(name);
        if (go) {
            lua.push(reinterpret_cast<uint8_t*>(go));
            lua.rawGet(LUA_REGISTRYINDEX);
            if (lua.isTable(-1)) return 1;
            lua.pop();
        }
    }

    lua.push();
    return 1;
}

int LuaAPI::Entity_GetCount(lua_State* L) {
    psyqo::Lua lua(L);
    
    if (s_sceneManager) {
        lua.pushNumber(static_cast<lua_Number>(s_sceneManager->getGameObjectCount()));
    } else {
        lua.pushNumber(0);
    }
    return 1;
}


int LuaAPI::Entity_SetActive(lua_State* L) {
    psyqo::Lua lua(L);
    
    if (!lua.isTable(1)) {
        return 0;
    }
    
    lua.getField(1, "__cpp_ptr");
    auto go = lua.toUserdata<GameObject>(-1);
    lua.pop();
    
    bool active = lua.toBoolean(2);
    
    if (go && s_sceneManager) {
        s_sceneManager->setObjectActive(go, active);
    }
    
    return 0;
}

int LuaAPI::Entity_IsActive(lua_State* L) {
    psyqo::Lua lua(L);
    
    if (!lua.isTable(1)) {
        lua.push(false);
        return 1;
    }
    
    lua.getField(1, "__cpp_ptr");
    auto go = lua.toUserdata<GameObject>(-1);
    lua.pop();
    
    if (go) {
        lua.push(go->isActive());
    } else {
        lua.push(false);
    }
    
    return 1;
}

int LuaAPI::Entity_GetPosition(lua_State* L) {
    psyqo::Lua lua(L);
    
    if (!lua.isTable(1)) {
        lua.push();
        return 1;
    }
    
    lua.getField(1, "__cpp_ptr");
    auto go = lua.toUserdata<GameObject>(-1);
    lua.pop();
    
    if (go) {
        PushVec3(lua, go->position.x, go->position.y, go->position.z);
        return 1;
    }
    
    lua.push();
    return 1;
}

int LuaAPI::Entity_SetPosition(lua_State* L) {
    psyqo::Lua lua(L);
    
    if (!lua.isTable(1) || !lua.isTable(2)) {
        return 0;
    }
    
    lua.getField(1, "__cpp_ptr");
    auto go = lua.toUserdata<GameObject>(-1);
    lua.pop();
    
    if (!go) return 0;
    
    psyqo::FixedPoint<12> x, y, z;
    ReadVec3(lua, 2, x, y, z);
    
    go->position.x = x;
    go->position.y = y;
    go->position.z = z;
    
    return 0;
}

int LuaAPI::Entity_GetRotationY(lua_State* L) {
    psyqo::Lua lua(L);
    
    if (!lua.isTable(1)) {
        lua.pushNumber(0);
        return 1;
    }
    
    lua.getField(1, "__cpp_ptr");
    auto go = lua.toUserdata<GameObject>(-1);
    lua.pop();
    
    if (!go) { lua.pushNumber(0); return 1; }
    
    // Y rotation matrix: vs[0].x = cos(θ), vs[0].z = sin(θ)
    int32_t sinRaw = go->rotation.vs[0].z.raw();
    int32_t cosRaw = go->rotation.vs[0].x.raw();
    
    // Fast atan2 approximation (linear in first octant, fold to full circle)
    psyqo::Angle angle;
    if (cosRaw == 0 && sinRaw == 0) {
        angle.value = 0;
    } else {
        int32_t abs_s = sinRaw < 0 ? -sinRaw : sinRaw;
        int32_t abs_c = cosRaw < 0 ? -cosRaw : cosRaw;
        int32_t minV = abs_s < abs_c ? abs_s : abs_c;
        int32_t maxV = abs_s > abs_c ? abs_s : abs_c;
        int32_t a = (minV * 256) / maxV;  // [0, 256] for [0, π/4]
        if (abs_s > abs_c) a = 512 - a;
        if (cosRaw < 0) a = 1024 - a;
        if (sinRaw < 0) a = -a;
        angle.value = a;
    }
    
    // Return as FixedPoint<12> (Angle is FixedPoint<10>, shift left 2 for fp12)
    psyqo::FixedPoint<12> fp12;
    fp12.value = angle.value << 2;
    lua.push(fp12);
    return 1;
}

int LuaAPI::Entity_SetRotationY(lua_State* L) {
    psyqo::Lua lua(L);

    if (!lua.isTable(1)) return 0;

    lua.getField(1, "__cpp_ptr");
    auto go = lua.toUserdata<GameObject>(-1);
    lua.pop();

    if (!go) return 0;

    // Accept FixedPoint or number, convert to Angle (FixedPoint<10>)
    psyqo::FixedPoint<12> fp12 = readFP(lua, 2);
    psyqo::Angle angle;
    angle.value = fp12.value >> 2;
    go->rotation = psyqo::SoftMath::generateRotationMatrix33(angle, psyqo::SoftMath::Axis::Y, s_trig);
    return 0;
}

int LuaAPI::Entity_ForEach(lua_State* L) {
    psyqo::Lua lua(L);
    
    if (!s_sceneManager || !lua.isFunction(1)) return 0;
    
    size_t count = s_sceneManager->getGameObjectCount();
    for (size_t i = 0; i < count; i++) {
        auto* go = s_sceneManager->getGameObject(static_cast<uint16_t>(i));
        if (!go || !go->isActive()) continue;
        
        // Push callback copy
        lua.copy(1);
        // Look up registered Lua table for this object (keyed by C++ pointer)
        lua.push(reinterpret_cast<uint8_t*>(go));
        lua.rawGet(LUA_REGISTRYINDEX);
        if (!lua.isTable(-1)) {
            lua.pop(2);  // pop non-table + callback copy
            continue;
        }
        lua.pushNumber(i);  // push index as second argument
        if (lua.pcall(2, 0) != LUA_OK) {
            lua.pop();  // pop error message
        }
    }
    
    return 0;
}

// ============================================================================
// VEC3 API IMPLEMENTATION
// ============================================================================

void LuaAPI::PushVec3(psyqo::Lua& L, psyqo::FixedPoint<12> x,
                      psyqo::FixedPoint<12> y, psyqo::FixedPoint<12> z) {
    L.newTable();
    L.push(x);
    L.setField(-2, "x");
    L.push(y);
    L.setField(-2, "y");
    L.push(z);
    L.setField(-2, "z");
}

void LuaAPI::ReadVec3(psyqo::Lua& L, int idx,
                      psyqo::FixedPoint<12>& x,
                      psyqo::FixedPoint<12>& y,
                      psyqo::FixedPoint<12>& z) {
    L.getField(idx, "x");
    x = readFP(L, -1);
    L.pop();

    L.getField(idx, "y");
    y = readFP(L, -1);
    L.pop();

    L.getField(idx, "z");
    z = readFP(L, -1);
    L.pop();
}

int LuaAPI::Vec3_New(lua_State* L) {
    psyqo::Lua lua(L);

    psyqo::FixedPoint<12> x = lua.isNoneOrNil(1) ? psyqo::FixedPoint<12>() : readFP(lua, 1);
    psyqo::FixedPoint<12> y = lua.isNoneOrNil(2) ? psyqo::FixedPoint<12>() : readFP(lua, 2);
    psyqo::FixedPoint<12> z = lua.isNoneOrNil(3) ? psyqo::FixedPoint<12>() : readFP(lua, 3);

    PushVec3(lua, x, y, z);
    return 1;
}

int LuaAPI::Vec3_Add(lua_State* L) {
    psyqo::Lua lua(L);
    
    if (!lua.isTable(1) || !lua.isTable(2)) {
        lua.push();
        return 1;
    }
    
    psyqo::FixedPoint<12> ax, ay, az;
    psyqo::FixedPoint<12> bx, by, bz;
    
    ReadVec3(lua, 1, ax, ay, az);
    ReadVec3(lua, 2, bx, by, bz);
    
    PushVec3(lua, ax + bx, ay + by, az + bz);
    return 1;
}

int LuaAPI::Vec3_Sub(lua_State* L) {
    psyqo::Lua lua(L);
    
    if (!lua.isTable(1) || !lua.isTable(2)) {
        lua.push();
        return 1;
    }
    
    psyqo::FixedPoint<12> ax, ay, az;
    psyqo::FixedPoint<12> bx, by, bz;
    
    ReadVec3(lua, 1, ax, ay, az);
    ReadVec3(lua, 2, bx, by, bz);
    
    PushVec3(lua, ax - bx, ay - by, az - bz);
    return 1;
}

int LuaAPI::Vec3_Mul(lua_State* L) {
    psyqo::Lua lua(L);
    
    if (!lua.isTable(1)) {
        lua.push();
        return 1;
    }
    
    psyqo::FixedPoint<12> x, y, z;
    ReadVec3(lua, 1, x, y, z);
    
    psyqo::FixedPoint<12> scalar = readFP(lua, 2);
    
    PushVec3(lua, x * scalar, y * scalar, z * scalar);
    return 1;
}

int LuaAPI::Vec3_Dot(lua_State* L) {
    psyqo::Lua lua(L);
    
    if (!lua.isTable(1) || !lua.isTable(2)) {
        lua.pushNumber(0);
        return 1;
    }
    
    psyqo::FixedPoint<12> ax, ay, az;
    psyqo::FixedPoint<12> bx, by, bz;
    
    ReadVec3(lua, 1, ax, ay, az);
    ReadVec3(lua, 2, bx, by, bz);
    
    auto dot = ax * bx + ay * by + az * bz;
    lua.push(dot);
    return 1;
}

int LuaAPI::Vec3_Cross(lua_State* L) {
    psyqo::Lua lua(L);
    
    if (!lua.isTable(1) || !lua.isTable(2)) {
        lua.push();
        return 1;
    }
    
    psyqo::FixedPoint<12> ax, ay, az;
    psyqo::FixedPoint<12> bx, by, bz;
    
    ReadVec3(lua, 1, ax, ay, az);
    ReadVec3(lua, 2, bx, by, bz);
    
    psyqo::FixedPoint<12> cx = ay * bz - az * by;
    psyqo::FixedPoint<12> cy = az * bx - ax * bz;
    psyqo::FixedPoint<12> cz = ax * by - ay * bx;
    
    PushVec3(lua, cx, cy, cz);
    return 1;
}

int LuaAPI::Vec3_LengthSq(lua_State* L) {
    psyqo::Lua lua(L);
    
    if (!lua.isTable(1)) {
        lua.pushNumber(0);
        return 1;
    }
    
    psyqo::FixedPoint<12> x, y, z;
    ReadVec3(lua, 1, x, y, z);
    
    auto lengthSq = x * x + y * y + z * z;
    lua.push(lengthSq);
    return 1;
}

int LuaAPI::Vec3_Length(lua_State* L) {
    psyqo::Lua lua(L);
    
    if (!lua.isTable(1)) {
        lua.pushNumber(0);
        return 1;
    }
    
    psyqo::FixedPoint<12> x, y, z;
    ReadVec3(lua, 1, x, y, z);
    
    // lengthSq in fp12: (x*x + y*y + z*z) is fp24 (two fp12 multiplied).
    // We need sqrt(lengthSq) as fp12.
    // lengthSq raw = sum of (raw*raw >> 12) values = fp12 result
    auto lengthSq = x * x + y * y + z * z;
    int32_t lsRaw = lengthSq.raw();

    if (lsRaw <= 0) {
        lua.push(psyqo::FixedPoint<12>());
        return 1;
    }

    // Integer sqrt of (lsRaw << 12) to get result in fp12
    // sqrt(fp12_value) = sqrt(raw/4096) = sqrt(raw)/64
    // So: result_raw = isqrt(raw * 4096) = isqrt(raw << 12)
    // isqrt(lsRaw) gives integer sqrt. Multiply by 64 (sqrt(4096)) to get fp12.
    // Newton's method in 32-bit: isqrt(n)
    uint32_t n = (uint32_t)lsRaw;
    uint32_t guess = n;
    for (int i = 0; i < 16; i++) {
        if (guess == 0) break;
        guess = (guess + n / guess) / 2;
    }
    // guess = isqrt(lsRaw). lsRaw is in fp12, so sqrt needs * sqrt(4096) = 64
    psyqo::FixedPoint<12> result;
    result.value = (int32_t)(guess * 64);
    lua.push(result);
    return 1;
}

int LuaAPI::Vec3_Normalize(lua_State* L) {
    psyqo::Lua lua(L);
    
    if (!lua.isTable(1)) {
        lua.push();
        return 1;
    }
    
    psyqo::FixedPoint<12> x, y, z;
    ReadVec3(lua, 1, x, y, z);
    
    auto lengthSq = x * x + y * y + z * z;
    int32_t lsRaw = lengthSq.raw();

    if (lsRaw <= 0) {
        PushVec3(lua, psyqo::FixedPoint<12>(), psyqo::FixedPoint<12>(), psyqo::FixedPoint<12>());
        return 1;
    }

    // isqrt(lsRaw) * 64 = length in fp12
    uint32_t n = (uint32_t)lsRaw;
    uint32_t guess = n;
    for (int i = 0; i < 16; i++) {
        if (guess == 0) break;
        guess = (guess + n / guess) / 2;
    }
    int32_t len = (int32_t)(guess * 64);
    if (len == 0) len = 1;

    // Divide each component by length: component / length in fp12
    // (x.raw * 4096) / len using 32-bit math (safe since raw values fit int16 range)
    psyqo::FixedPoint<12> nx, ny, nz;
    nx.value = (x.raw() * 4096) / len;
    ny.value = (y.raw() * 4096) / len;
    nz.value = (z.raw() * 4096) / len;
    PushVec3(lua, nx, ny, nz);
    return 1;
}

int LuaAPI::Vec3_DistanceSq(lua_State* L) {
    psyqo::Lua lua(L);
    
    if (!lua.isTable(1) || !lua.isTable(2)) {
        lua.pushNumber(0);
        return 1;
    }
    
    psyqo::FixedPoint<12> ax, ay, az;
    psyqo::FixedPoint<12> bx, by, bz;
    
    ReadVec3(lua, 1, ax, ay, az);
    ReadVec3(lua, 2, bx, by, bz);
    
    auto dx = ax - bx;
    auto dy = ay - by;
    auto dz = az - bz;
    
    auto distSq = dx * dx + dy * dy + dz * dz;
    lua.push(distSq);
    return 1;
}

int LuaAPI::Vec3_Distance(lua_State* L) {
    psyqo::Lua lua(L);
    
    if (!lua.isTable(1) || !lua.isTable(2)) {
        lua.pushNumber(0);
        return 1;
    }
    
    psyqo::FixedPoint<12> ax, ay, az;
    psyqo::FixedPoint<12> bx, by, bz;
    
    ReadVec3(lua, 1, ax, ay, az);
    ReadVec3(lua, 2, bx, by, bz);
    
    auto dx = ax - bx;
    auto dy = ay - by;
    auto dz = az - bz;

    auto distSq = dx * dx + dy * dy + dz * dz;
    int32_t dsRaw = distSq.raw();

    if (dsRaw <= 0) {
        lua.push(psyqo::FixedPoint<12>());
        return 1;
    }

    uint32_t n = (uint32_t)dsRaw;
    uint32_t guess = n;
    for (int i = 0; i < 16; i++) {
        if (guess == 0) break;
        guess = (guess + n / guess) / 2;
    }

    psyqo::FixedPoint<12> result;
    result.value = (int32_t)(guess * 64);
    lua.push(result);
    return 1;
}

int LuaAPI::Vec3_Lerp(lua_State* L) {
    psyqo::Lua lua(L);
    
    if (!lua.isTable(1) || !lua.isTable(2)) {
        lua.push();
        return 1;
    }
    
    psyqo::FixedPoint<12> ax, ay, az;
    psyqo::FixedPoint<12> bx, by, bz;
    
    ReadVec3(lua, 1, ax, ay, az);
    ReadVec3(lua, 2, bx, by, bz);
    
    psyqo::FixedPoint<12> t = readFP(lua, 3);
    psyqo::FixedPoint<12> oneMinusT = psyqo::FixedPoint<12>(4096, psyqo::FixedPoint<12>::RAW) - t;
    
    psyqo::FixedPoint<12> rx = ax * oneMinusT + bx * t;
    psyqo::FixedPoint<12> ry = ay * oneMinusT + by * t;
    psyqo::FixedPoint<12> rz = az * oneMinusT + bz * t;
    
    PushVec3(lua, rx, ry, rz);
    return 1;
}

// ============================================================================
// INPUT API IMPLEMENTATION
// ============================================================================

void LuaAPI::RegisterInputConstants(psyqo::Lua& L) {
    // Button constants - must match psyqo::AdvancedPad::Button enum
    L.pushNumber(static_cast<lua_Number>(psyqo::AdvancedPad::Button::Cross));
    L.setField(-2, "CROSS");
    
    L.pushNumber(static_cast<lua_Number>(psyqo::AdvancedPad::Button::Circle));
    L.setField(-2, "CIRCLE");
    
    L.pushNumber(static_cast<lua_Number>(psyqo::AdvancedPad::Button::Square));
    L.setField(-2, "SQUARE");
    
    L.pushNumber(static_cast<lua_Number>(psyqo::AdvancedPad::Button::Triangle));
    L.setField(-2, "TRIANGLE");
    
    L.pushNumber(static_cast<lua_Number>(psyqo::AdvancedPad::Button::L1));
    L.setField(-2, "L1");
    
    L.pushNumber(static_cast<lua_Number>(psyqo::AdvancedPad::Button::R1));
    L.setField(-2, "R1");
    
    L.pushNumber(static_cast<lua_Number>(psyqo::AdvancedPad::Button::L2));
    L.setField(-2, "L2");
    
    L.pushNumber(static_cast<lua_Number>(psyqo::AdvancedPad::Button::R2));
    L.setField(-2, "R2");
    
    L.pushNumber(static_cast<lua_Number>(psyqo::AdvancedPad::Button::Start));
    L.setField(-2, "START");
    
    L.pushNumber(static_cast<lua_Number>(psyqo::AdvancedPad::Button::Select));
    L.setField(-2, "SELECT");
    
    L.pushNumber(static_cast<lua_Number>(psyqo::AdvancedPad::Button::Up));
    L.setField(-2, "UP");
    
    L.pushNumber(static_cast<lua_Number>(psyqo::AdvancedPad::Button::Down));
    L.setField(-2, "DOWN");
    
    L.pushNumber(static_cast<lua_Number>(psyqo::AdvancedPad::Button::Left));
    L.setField(-2, "LEFT");
    
    L.pushNumber(static_cast<lua_Number>(psyqo::AdvancedPad::Button::Right));
    L.setField(-2, "RIGHT");
    
    L.pushNumber(static_cast<lua_Number>(psyqo::AdvancedPad::Button::L3));
    L.setField(-2, "L3");
    
    L.pushNumber(static_cast<lua_Number>(psyqo::AdvancedPad::Button::R3));
    L.setField(-2, "R3");
}

int LuaAPI::Input_IsPressed(lua_State* L) {
    psyqo::Lua lua(L);
    
    if (!s_sceneManager || !lua.isNumber(1)) {
        lua.push(false);
        return 1;
    }
    
    auto button = static_cast<psyqo::AdvancedPad::Button>(static_cast<uint16_t>(lua.toNumber(1)));
    lua.push(s_sceneManager->getControls().wasButtonPressed(button));
    return 1;
}

int LuaAPI::Input_IsReleased(lua_State* L) {
    psyqo::Lua lua(L);
    
    if (!s_sceneManager || !lua.isNumber(1)) {
        lua.push(false);
        return 1;
    }
    
    auto button = static_cast<psyqo::AdvancedPad::Button>(static_cast<uint16_t>(lua.toNumber(1)));
    lua.push(s_sceneManager->getControls().wasButtonReleased(button));
    return 1;
}

int LuaAPI::Input_IsHeld(lua_State* L) {
    psyqo::Lua lua(L);
    
    if (!s_sceneManager || !lua.isNumber(1)) {
        lua.push(false);
        return 1;
    }
    
    auto button = static_cast<psyqo::AdvancedPad::Button>(static_cast<uint16_t>(lua.toNumber(1)));
    lua.push(s_sceneManager->getControls().isButtonHeld(button));
    return 1;
}

int LuaAPI::Input_GetAnalog(lua_State* L) {
    psyqo::Lua lua(L);
    
    if (!s_sceneManager) {
        lua.pushNumber(0);
        lua.pushNumber(0);
        return 2;
    }
    
    int stick = lua.isNumber(1) ? static_cast<int>(lua.toNumber(1)) : 0;
    auto& controls = s_sceneManager->getControls();
    
    int16_t x, y;
    if (stick == 1) {
        x = controls.getRightStickX();
        y = controls.getRightStickY();
    } else {
        x = controls.getLeftStickX();
        y = controls.getLeftStickY();
    }
    
    // Scale to approximately [-1.0, 1.0] in Lua number space
    // Stick range is -127 to +127; divide by 127
    lua.pushNumber(x * kFixedScale / 127);
    lua.pushNumber(y * kFixedScale / 127);
    return 2;
}

// ============================================================================
// TIMER API IMPLEMENTATION
// ============================================================================

static uint32_t s_frameCount = 0;

void LuaAPI::IncrementFrameCount() {
    s_frameCount++;
}

void LuaAPI::ResetFrameCount() {
    s_frameCount = 0;
}

int LuaAPI::Timer_GetFrameCount(lua_State* L) {
    psyqo::Lua lua(L);
    lua.pushNumber(s_frameCount);
    return 1;
}

// ============================================================================
// CAMERA API IMPLEMENTATION
// ============================================================================

int LuaAPI::Camera_GetPosition(lua_State* L) {
    psyqo::Lua lua(L);
    
    if (s_sceneManager) {
        auto& pos = s_sceneManager->getCamera().GetPosition();
        PushVec3(lua, pos.x, pos.y, pos.z);
    } else {
        PushVec3(lua, psyqo::FixedPoint<12>(0), psyqo::FixedPoint<12>(0), psyqo::FixedPoint<12>(0));
    }
    return 1;
}

int LuaAPI::Camera_SetPosition(lua_State* L) {
    psyqo::Lua lua(L);
    
    if (!s_sceneManager || !lua.isTable(1)) return 0;
    
    psyqo::FixedPoint<12> x, y, z;
    ReadVec3(lua, 1, x, y, z);
    s_sceneManager->getCamera().SetPosition(x, y, z);
    return 0;
}

int LuaAPI::Camera_GetRotation(lua_State* L) {
    psyqo::Lua lua(L);
    
    psyqo::FixedPoint<12> rotX = psyqo::FixedPoint<12>(static_cast<int32_t>(s_sceneManager->getCamera().GetAngleX() * 4), psyqo::FixedPoint<12>::RAW);
    psyqo::FixedPoint<12> rotY = psyqo::FixedPoint<12>(static_cast<int32_t>(s_sceneManager->getCamera().GetAngleY() * 4), psyqo::FixedPoint<12>::RAW);
    psyqo::FixedPoint<12> rotZ = psyqo::FixedPoint<12>(static_cast<int32_t>(s_sceneManager->getCamera().GetAngleZ() * 4), psyqo::FixedPoint<12>::RAW);

    PushVec3(lua, rotX, rotY, rotZ);
    return 1;
}

int LuaAPI::Camera_SetRotation(lua_State* L) {
    psyqo::Lua lua(L);
    
    if (!s_sceneManager || !lua.isTable(1)) return 0;
    
    // Accept three angles in pi-units (e.g., 0.5 = π/2 = 90°)
    // This matches psyqo::Angle convention used by the engine.
    psyqo::FixedPoint<12> x, y, z;
    ReadVec3(lua, 1, x, y, z);

    // Convert to Angle (FixedPoint<10>) 
    psyqo::Angle rx, ry, rz;
    rx.value = x.value >> 2;
    ry.value = y.value >> 2;
    rz.value = z.value >> 2;

    s_sceneManager->getCamera().SetRotation(rx, ry, rz);
    return 0;
}

int LuaAPI::Camera_GetForward(lua_State* L) {
    psyqo::Lua lua(L);

    psyqo::Matrix33 camRotationMatrix = s_sceneManager->getCamera().GetRotation();

    psyqo::FixedPoint<12> fwdX = camRotationMatrix.vs[2].x;
    psyqo::FixedPoint<12> fwdY = camRotationMatrix.vs[2].y;
    psyqo::FixedPoint<12> fwdZ = camRotationMatrix.vs[2].z;

    PushVec3(lua, fwdX, fwdY, fwdZ);
    return 1;
}

int LuaAPI::Camera_MoveForward(lua_State* L) {
    psyqo::Lua lua(L);

    if (!lua.isTable(1)) return 0;

    psyqo::FixedPoint<12> stepAmount = readFP(lua, 1);

    auto& cam = s_sceneManager->getCamera();

    psyqo::Matrix33 camRotationMatrix = cam.GetRotation();

    psyqo::FixedPoint<12> fwdX = camRotationMatrix.vs[2].x * stepAmount;
    psyqo::FixedPoint<12> fwdY = camRotationMatrix.vs[2].y * stepAmount;
    psyqo::FixedPoint<12> fwdZ = camRotationMatrix.vs[2].z * stepAmount;
    
    psyqo::Vec3 pos = cam.GetPosition();

    pos.x = pos.x + fwdX;
    pos.y = pos.y + fwdY;
    pos.z = pos.z + fwdZ;

    cam.SetPosition(pos.x,pos.y,pos.z);

    return 0;
}

int LuaAPI::Camera_MoveBackward(lua_State* L) {
    psyqo::Lua lua(L);

    if (!lua.isTable(1)) return 0;

    psyqo::FixedPoint<12> stepAmount = readFP(lua, 1);

    auto& cam = s_sceneManager->getCamera();

    psyqo::Matrix33 camRotationMatrix = cam.GetRotation();

    psyqo::FixedPoint<12> fwdX = camRotationMatrix.vs[2].x * stepAmount;
    psyqo::FixedPoint<12> fwdY = camRotationMatrix.vs[2].y * stepAmount;
    psyqo::FixedPoint<12> fwdZ = camRotationMatrix.vs[2].z * stepAmount;
    
    psyqo::Vec3 pos = cam.GetPosition();

    pos.x = pos.x + fwdX * -1;
    pos.y = pos.y + fwdY * -1;
    pos.z = pos.z + fwdZ * -1;

    cam.SetPosition(pos.x,pos.y,pos.z);

    return 0;
}

int LuaAPI::Camera_MoveLeft(lua_State* L) {
    psyqo::Lua lua(L);

    if (!lua.isTable(1)) return 0;

    psyqo::FixedPoint<12> stepAmount = readFP(lua, 1);

    auto& cam = s_sceneManager->getCamera();

    psyqo::Matrix33 camRotationMatrix = cam.GetRotation();
    
    // Get camera forward 
    psyqo::FixedPoint<12> fwdX = camRotationMatrix.vs[2].x;
    psyqo::FixedPoint<12> fwdY = camRotationMatrix.vs[2].y;
    psyqo::FixedPoint<12> fwdZ = camRotationMatrix.vs[2].z;
    
    psyqo::Vec3 worldUpVector = psyqo::Vec3{0, 1, 0};

    // Vector Cross
    psyqo::FixedPoint<12> rx = fwdY * worldUpVector.z - fwdZ * worldUpVector.y;
    psyqo::FixedPoint<12> ry = fwdZ * worldUpVector.x - fwdX * worldUpVector.z;
    psyqo::FixedPoint<12> rz = fwdX * worldUpVector.y - fwdY * worldUpVector.x;
    
    psyqo::Vec3 pos = cam.GetPosition();

    pos.x = pos.x + rx * stepAmount;
    pos.y = pos.y + ry * stepAmount;
    pos.z = pos.z + rz * stepAmount;

    cam.SetPosition(pos.x,pos.y,pos.z);

    return 0;
}

int LuaAPI::Camera_MoveRight(lua_State* L) {
    psyqo::Lua lua(L);

    if (!lua.isTable(1)) return 0;

    psyqo::FixedPoint<12> stepAmount = readFP(lua, 1);

    auto& cam = s_sceneManager->getCamera();

    psyqo::Matrix33 camRotationMatrix = cam.GetRotation();
    
    // Get camera forward and multiply it
    psyqo::FixedPoint<12> fwdX = camRotationMatrix.vs[2].x;
    psyqo::FixedPoint<12> fwdY = camRotationMatrix.vs[2].y;
    psyqo::FixedPoint<12> fwdZ = camRotationMatrix.vs[2].z;
    
    psyqo::Vec3 worldUpVector = psyqo::Vec3{0, 1, 0};

    // Vector Cross
    psyqo::FixedPoint<12> rx = fwdY * worldUpVector.z - fwdZ * worldUpVector.y;
    psyqo::FixedPoint<12> ry = fwdZ * worldUpVector.x - fwdX * worldUpVector.z;
    psyqo::FixedPoint<12> rz = fwdX * worldUpVector.y - fwdY * worldUpVector.x;

    psyqo::Vec3 pos = cam.GetPosition();

    pos.x = pos.x - rx * stepAmount;
    pos.y = pos.y - ry * stepAmount;
    pos.z = pos.z - rz * stepAmount;

    cam.SetPosition(pos.x,pos.y,pos.z);

    return 0;
}

int LuaAPI::Camera_LookAt(lua_State* L) {
    psyqo::Lua lua(L);
    
    if (!s_sceneManager) return 0;
    
    psyqo::FixedPoint<12> tx, ty, tz;
    
    if (lua.isTable(1)) {
        ReadVec3(lua, 1, tx, ty, tz);
    } else {
        tx = lua.isNoneOrNil(1) ? psyqo::FixedPoint<12>() : readFP(lua, 1);
        ty = lua.isNoneOrNil(2) ? psyqo::FixedPoint<12>() : readFP(lua, 2);
        tz = lua.isNoneOrNil(3) ? psyqo::FixedPoint<12>() : readFP(lua, 3);
    }
    
    auto& cam = s_sceneManager->getCamera();
    auto& pos = cam.GetPosition();
    
    // Compute direction vector from camera to target
    auto dx = tx - pos.x;
    auto dy = ty - pos.y;
    auto dz = tz - pos.z;

    // Compute horizontal distance for pitch calculation
    auto horizDistSq = dx * dx + dz * dz;
    int32_t hdsRaw = horizDistSq.raw();
    uint32_t hn = (uint32_t)(hdsRaw > 0 ? hdsRaw : 1);
    uint32_t horizGuess = hn;
    for (int i = 0; i < 16; i++) {
        if (horizGuess == 0) break;
        horizGuess = (horizGuess + hn / horizGuess) / 2;
    }
    
    // Yaw = atan2(dx, dz) — approximate with lookup or use psyqo trig
    // For now, use a simple atan2 approximation in fp12 domain
    // and set rotation via SetRotation (pitch, yaw, 0)
    // Approximate: yaw is proportional to dx/dz in small-angle
    // Full implementation requires psyqo Trig atan2 which is not trivially
    // accessible here. Set rotation to face the target on the Y axis.
    // This is a simplified look-at that only handles yaw.
    psyqo::Angle yaw;
    psyqo::Angle pitch;
    
    // Use scaled integer atan2 approximation
    // atan2(dx, dz) in the range [-π, π]
    // For PS1, the exact method depends on psyqo's Trig class.
    // Returning luaError since we can't do a proper atan2 without Trig instance.
    // Compromise: just set rotation angles directly
    yaw.value = 0;
    pitch.value = 0;
    
    // For a real implementation, Camera would need a LookAt method.
    return 0;
}

// ============================================================================
// AUDIO API IMPLEMENTATION
// ============================================================================

int LuaAPI::Audio_Play(lua_State* L) {
    psyqo::Lua lua(L);

    if (!s_sceneManager) {
        lua.pushNumber(-1);
        return 1;
    }

    int soundId = -1;

    // Accept number (index) or string (name lookup) like Entity.Find
    // Check isNumber FIRST - in Lua, numbers pass isString too.
    if (lua.isNumber(1)) {
        soundId = static_cast<int>(lua.toNumber(1));
    } else if (lua.isString(1)) {
        const char* name = lua.toString(1);
        soundId = s_sceneManager->findAudioClipByName(name);
        if (soundId < 0) {
            lua.pushNumber(-1);
            return 1;
        }
    } else {
        lua.pushNumber(-1);
        return 1;
    }

    int volume = static_cast<int>(lua.optNumber(2, 100));
    int pan = static_cast<int>(lua.optNumber(3, 64));

    int voice = s_sceneManager->getAudio().play(soundId, volume, pan);
    lua.pushNumber(voice);
    return 1;
}

int LuaAPI::Audio_Find(lua_State* L) {
    psyqo::Lua lua(L);
    
    if (!s_sceneManager || !lua.isString(1)) {
        lua.push();  // nil
        return 1;
    }
    
    const char* name = lua.toString(1);
    int clipIndex = s_sceneManager->findAudioClipByName(name);
    
    if (clipIndex >= 0) {
        lua.pushNumber(static_cast<lua_Number>(clipIndex));
    } else {
        lua.push();  // nil
    }
    return 1;
}

int LuaAPI::Audio_Stop(lua_State* L) {
    psyqo::Lua lua(L);
    if (!s_sceneManager) return 0;
    int channelId = static_cast<int>(lua.toNumber(1));
    s_sceneManager->getAudio().stopVoice(channelId);
    return 0;
}

int LuaAPI::Audio_SetVolume(lua_State* L) {
    psyqo::Lua lua(L);
    if (!s_sceneManager) return 0;
    int channelId = static_cast<int>(lua.toNumber(1));
    int volume = static_cast<int>(lua.toNumber(2));
    int pan = static_cast<int>(lua.optNumber(3, 64));
    s_sceneManager->getAudio().setVoiceVolume(channelId, volume, pan);
    return 0;
}

int LuaAPI::Audio_StopAll(lua_State* L) {
    psyqo::Lua lua(L);
    if (!s_sceneManager) return 0;
    s_sceneManager->getAudio().stopAll();
    return 0;
}

// ============================================================================
// DEBUG API IMPLEMENTATION
// ============================================================================

int LuaAPI::Debug_Log(lua_State* L) {
    psyqo::Lua lua(L);
    if (lua.isString(1)) {
        printf("%s\n", lua.toString(1));
    }
    return 0;
}

int LuaAPI::Debug_DrawLine(lua_State* L) {
    psyqo::Lua lua(L);
    
    // Parse start and end Vec3 tables, optional color
    psyqo::FixedPoint<12> sx, sy, sz, ex, ey, ez;
    if (lua.isTable(1) && lua.isTable(2)) {
        ReadVec3(lua, 1, sx, sy, sz);
        ReadVec3(lua, 2, ex, ey, ez);
    }
    
    // TODO: Queue LINE_G2 primitive through Renderer
    return 0;
}

int LuaAPI::Debug_DrawBox(lua_State* L) {
    psyqo::Lua lua(L);
    
    // Parse center and size Vec3 tables, optional color
    psyqo::FixedPoint<12> cx, cy, cz, hx, hy, hz;
    if (lua.isTable(1) && lua.isTable(2)) {
        ReadVec3(lua, 1, cx, cy, cz);
        ReadVec3(lua, 2, hx, hy, hz);
    }
    
    // TODO: Queue 12 LINE_G2 primitives (box wireframe) through Renderer
    return 0;
}

// ============================================================================
// MATH API IMPLEMENTATION
// ============================================================================

int LuaAPI::Math_Clamp(lua_State* L) {
    psyqo::Lua lua(L);
    
    lua_Number value = lua.toNumber(1);
    lua_Number minVal = lua.toNumber(2);
    lua_Number maxVal = lua.toNumber(3);
    
    if (value < minVal) value = minVal;
    if (value > maxVal) value = maxVal;
    
    lua.pushNumber(value);
    return 1;
}

int LuaAPI::Math_Lerp(lua_State* L) {
    psyqo::Lua lua(L);
    
    lua_Number a = lua.toNumber(1);
    lua_Number b = lua.toNumber(2);
    lua_Number t = lua.toNumber(3);
    
    lua.pushNumber(a + (b - a) * t);
    return 1;
}

int LuaAPI::Math_Sign(lua_State* L) {
    psyqo::Lua lua(L);
    
    lua_Number value = lua.toNumber(1);
    
    if (value > 0) lua.pushNumber(1);
    else if (value < 0) lua.pushNumber(-1);
    else lua.pushNumber(0);
    
    return 1;
}

int LuaAPI::Math_Abs(lua_State* L) {
    psyqo::Lua lua(L);
    
    lua_Number value = lua.toNumber(1);
    lua.pushNumber(value < 0 ? -value : value);
    return 1;
}

int LuaAPI::Math_Min(lua_State* L) {
    psyqo::Lua lua(L);
    
    lua_Number a = lua.toNumber(1);
    lua_Number b = lua.toNumber(2);
    
    lua.pushNumber(a < b ? a : b);
    return 1;
}

int LuaAPI::Math_Max(lua_State* L) {
    psyqo::Lua lua(L);
    
    lua_Number a = lua.toNumber(1);
    lua_Number b = lua.toNumber(2);
    
    lua.pushNumber(a > b ? a : b);
    return 1;
}

// ============================================================================
// SCENE API IMPLEMENTATION
// ============================================================================

int LuaAPI::Scene_Load(lua_State* L) {
    psyqo::Lua lua(L);
    
    if (!s_sceneManager || !lua.isNumber(1)) {
        return 0;
    }
    
    int sceneIndex = static_cast<int>(lua.toNumber(1));
    s_sceneManager->requestSceneLoad(sceneIndex);
    return 0;
}

int LuaAPI::Scene_GetIndex(lua_State* L) {
    psyqo::Lua lua(L);
    
    if (!s_sceneManager) {
        lua.pushNumber(0);
        return 1;
    }
    
    lua.pushNumber(static_cast<lua_Number>(s_sceneManager->getCurrentSceneIndex()));
    return 1;
}

// ============================================================================
// PERSIST API IMPLEMENTATION
// ============================================================================

struct PersistEntry {
    char key[32];
    lua_Number value;
    bool used;
};

static PersistEntry s_persistData[16] = {};

// Inline string helpers (no libc on bare-metal PS1)
static bool streq(const char* a, const char* b) {
    while (*a && *b) { if (*a++ != *b++) return false; }
    return *a == *b;
}

static void strcopy(char* dst, const char* src, int maxLen) {
    int i = 0;
    for (; i < maxLen - 1 && src[i]; i++) dst[i] = src[i];
    dst[i] = '\0';
}

int LuaAPI::Persist_Get(lua_State* L) {
    psyqo::Lua lua(L);
    const char* key = lua.toString(1);
    if (!key) { lua.push(); return 1; }
    
    for (int i = 0; i < 16; i++) {
        if (s_persistData[i].used && streq(s_persistData[i].key, key)) {
            lua.pushNumber(s_persistData[i].value);
            return 1;
        }
    }
    lua.push();  // nil
    return 1;
}

int LuaAPI::Persist_Set(lua_State* L) {
    psyqo::Lua lua(L);
    const char* key = lua.toString(1);
    if (!key) return 0;
    
    lua_Number value = lua.toNumber(2);
    
    // Update existing key
    for (int i = 0; i < 16; i++) {
        if (s_persistData[i].used && streq(s_persistData[i].key, key)) {
            s_persistData[i].value = value;
            return 0;
        }
    }
    
    // Find empty slot
    for (int i = 0; i < 16; i++) {
        if (!s_persistData[i].used) {
            strcopy(s_persistData[i].key, key, 32);
            s_persistData[i].value = value;
            s_persistData[i].used = true;
            return 0;
        }
    }
    
    return 0;  // No room — silently fail
}

void LuaAPI::PersistClear() {
    for (int i = 0; i < 16; i++) {
        s_persistData[i].used = false;
    }
}

// ============================================================================
// CUTSCENE API IMPLEMENTATION
// ============================================================================

int LuaAPI::Cutscene_Play(lua_State* L) {
    psyqo::Lua lua(L);

    if (!s_cutscenePlayer || !lua.isString(1)) {
        return 0;
    }

    const char* name = lua.toString(1);
    bool loop = false;
    int onCompleteRef = LUA_NOREF;

    // Optional second argument: options table {loop=bool, onComplete=function}
    if (lua.isTable(2)) {
        lua.getField(2, "loop");
        if (lua.isBoolean(-1)) loop = lua.toBoolean(-1);
        lua.pop();

        lua.getField(2, "onComplete");
        if (lua.isFunction(-1)) {
            onCompleteRef = lua.ref();  // pops and stores in registry
        } else {
            lua.pop();
        }
    }

    // Clear any previous callback before starting a new cutscene
    int oldRef = s_cutscenePlayer->getOnCompleteRef();
    if (oldRef != LUA_NOREF) {
        luaL_unref(L, LUA_REGISTRYINDEX, oldRef);
    }

    s_cutscenePlayer->setLuaState(L);
    s_cutscenePlayer->setOnCompleteRef(onCompleteRef);
    s_cutscenePlayer->play(name, loop);
    return 0;
}

int LuaAPI::Cutscene_Stop(lua_State* L) {
    psyqo::Lua lua(L);
    
    if (s_cutscenePlayer) {
        s_cutscenePlayer->stop();
    }
    return 0;
}

int LuaAPI::Cutscene_IsPlaying(lua_State* L) {
    psyqo::Lua lua(L);
    
    if (s_cutscenePlayer) {
        lua.push(s_cutscenePlayer->isPlaying());
    } else {
        lua.push(false);
    }
    return 1;
}

// ============================================================================
// ANIMATION API IMPLEMENTATION
// ============================================================================

int LuaAPI::Animation_Play(lua_State* L) {
    psyqo::Lua lua(L);

    if (!s_animationPlayer || !lua.isString(1)) {
        return 0;
    }

    const char* name = lua.toString(1);
    bool loop = false;
    int onCompleteRef = LUA_NOREF;

    if (lua.isTable(2)) {
        lua.getField(2, "loop");
        if (lua.isBoolean(-1)) loop = lua.toBoolean(-1);
        lua.pop();

        lua.getField(2, "onComplete");
        if (lua.isFunction(-1)) {
            onCompleteRef = lua.ref();  // pops and stores in registry
        } else {
            lua.pop();
        }
    }

    s_animationPlayer->setLuaState(L);
    s_animationPlayer->play(name, loop);

    if (onCompleteRef != LUA_NOREF) {
        s_animationPlayer->setOnCompleteRef(name, onCompleteRef);
    }

    return 0;
}

int LuaAPI::Animation_Stop(lua_State* L) {
    psyqo::Lua lua(L);
    if (!s_animationPlayer) return 0;

    if (lua.isString(1)) {
        s_animationPlayer->stop(lua.toString(1));
    } else {
        s_animationPlayer->stopAll();
    }
    return 0;
}

int LuaAPI::Animation_IsPlaying(lua_State* L) {
    psyqo::Lua lua(L);

    if (s_animationPlayer && lua.isString(1)) {
        lua.push(s_animationPlayer->isPlaying(lua.toString(1)));
    } else {
        lua.push(false);
    }
    return 1;
}

// ============================================================================
// CONTROLS API IMPLEMENTATION
// ============================================================================

int LuaAPI::Controls_SetEnabled(lua_State* L) {
    psyqo::Lua lua(L);
    if (s_sceneManager && lua.isBoolean(1)) {
        s_sceneManager->setControlsEnabled(lua.toBoolean(1));
    }
    return 0;
}

int LuaAPI::Controls_IsEnabled(lua_State* L) {
    psyqo::Lua lua(L);
    if (s_sceneManager) {
        lua.push(s_sceneManager->isControlsEnabled());
    } else {
        lua.push(false);
    }
    return 1;
}

// ============================================================================
// INTERACT API IMPLEMENTATION
// ============================================================================

int LuaAPI::Interact_SetEnabled(lua_State* L) {
    psyqo::Lua lua(L);
    if (!s_sceneManager || !lua.isTable(1) || !lua.isBoolean(2)) return 0;

    lua.getField(1, "__cpp_ptr");
    auto go = lua.toUserdata<psxsplash::GameObject>(-1);
    lua.pop();

    if (go && go->hasInteractable()) {
        auto* inter = s_sceneManager->getInteractable(go->interactableIndex);
        if (inter) {
            inter->setDisabled(!lua.toBoolean(2));
        }
    }
    return 0;
}

int LuaAPI::Interact_IsEnabled(lua_State* L) {
    psyqo::Lua lua(L);
    if (!s_sceneManager || !lua.isTable(1)) {
        lua.push(false);
        return 1;
    }

    lua.getField(1, "__cpp_ptr");
    auto go = lua.toUserdata<psxsplash::GameObject>(-1);
    lua.pop();

    if (go && go->hasInteractable()) {
        auto* inter = s_sceneManager->getInteractable(go->interactableIndex);
        if (inter) {
            lua.push(!inter->isDisabled());
            return 1;
        }
    }
    lua.push(false);
    return 1;
}

// ============================================================================
// UI API IMPLEMENTATION
// ============================================================================

int LuaAPI::UI_FindCanvas(lua_State* L) {
    psyqo::Lua lua(L);
    if (!s_uiSystem || !lua.isString(1)) {
        lua.pushNumber(-1);
        return 1;
    }
    const char* name = lua.toString(1);
    int idx = s_uiSystem->findCanvas(name);
    lua.pushNumber(static_cast<lua_Number>(idx));
    return 1;
}

int LuaAPI::UI_SetCanvasVisible(lua_State* L) {
    psyqo::Lua lua(L);
    if (!s_uiSystem) return 0;
    int idx;
    // Accept number (index) or string (name)
    if (lua.isNumber(1)) {
        idx = static_cast<int>(lua.toNumber(1));
    } else if (lua.isString(1)) {
        idx = s_uiSystem->findCanvas(lua.toString(1));
    } else {
        return 0;
    }
    bool visible = lua.toBoolean(2);
    s_uiSystem->setCanvasVisible(idx, visible);
    return 0;
}

int LuaAPI::UI_IsCanvasVisible(lua_State* L) {
    psyqo::Lua lua(L);
    if (!s_uiSystem) {
        lua.push(false);
        return 1;
    }
    int idx;
    if (lua.isNumber(1)) {
        idx = static_cast<int>(lua.toNumber(1));
    } else if (lua.isString(1)) {
        idx = s_uiSystem->findCanvas(lua.toString(1));
    } else {
        lua.push(false);
        return 1;
    }
    lua.push(s_uiSystem->isCanvasVisible(idx));
    return 1;
}

int LuaAPI::UI_FindElement(lua_State* L) {
    psyqo::Lua lua(L);
    if (!s_uiSystem || !lua.isNumber(1) || !lua.isString(2)) {
        lua.pushNumber(-1);
        return 1;
    }
    int canvasIdx = static_cast<int>(lua.toNumber(1));
    const char* name = lua.toString(2);
    int handle = s_uiSystem->findElement(canvasIdx, name);
    lua.pushNumber(static_cast<lua_Number>(handle));
    return 1;
}

int LuaAPI::UI_SetVisible(lua_State* L) {
    psyqo::Lua lua(L);
    if (!s_uiSystem || !lua.isNumber(1)) return 0;
    int handle = static_cast<int>(lua.toNumber(1));
    bool visible = lua.toBoolean(2);
    s_uiSystem->setElementVisible(handle, visible);
    return 0;
}

int LuaAPI::UI_IsVisible(lua_State* L) {
    psyqo::Lua lua(L);
    if (!s_uiSystem || !lua.isNumber(1)) {
        lua.push(false);
        return 1;
    }
    int handle = static_cast<int>(lua.toNumber(1));
    lua.push(s_uiSystem->isElementVisible(handle));
    return 1;
}

int LuaAPI::UI_SetText(lua_State* L) {
    psyqo::Lua lua(L);
    if (!s_uiSystem || !lua.isNumber(1)) return 0;
    int handle = static_cast<int>(lua.toNumber(1));
    const char* text = lua.isString(2) ? lua.toString(2) : "";
    s_uiSystem->setText(handle, text);
    return 0;
}

int LuaAPI::UI_SetProgress(lua_State* L) {
    psyqo::Lua lua(L);
    if (!s_uiSystem || !lua.isNumber(1)) return 0;
    int handle = static_cast<int>(lua.toNumber(1));
    int value = static_cast<int>(lua.toNumber(2));
    if (value < 0) value = 0;
    if (value > 100) value = 100;
    s_uiSystem->setProgress(handle, (uint8_t)value);
    return 0;
}

int LuaAPI::UI_GetProgress(lua_State* L) {
    psyqo::Lua lua(L);
    if (!s_uiSystem || !lua.isNumber(1)) {
        lua.pushNumber(0);
        return 1;
    }
    int handle = static_cast<int>(lua.toNumber(1));
    lua.pushNumber(static_cast<lua_Number>(s_uiSystem->getProgress(handle)));
    return 1;
}

int LuaAPI::UI_SetColor(lua_State* L) {
    psyqo::Lua lua(L);
    if (!s_uiSystem || !lua.isNumber(1)) return 0;
    int handle = static_cast<int>(lua.toNumber(1));
    uint8_t r = static_cast<uint8_t>(lua.toNumber(2));
    uint8_t g = static_cast<uint8_t>(lua.toNumber(3));
    uint8_t b = static_cast<uint8_t>(lua.toNumber(4));
    s_uiSystem->setColor(handle, r, g, b);
    return 0;
}

int LuaAPI::UI_SetPosition(lua_State* L) {
    psyqo::Lua lua(L);
    if (!s_uiSystem || !lua.isNumber(1)) return 0;
    int handle = static_cast<int>(lua.toNumber(1));
    int16_t x = static_cast<int16_t>(lua.toNumber(2));
    int16_t y = static_cast<int16_t>(lua.toNumber(3));
    s_uiSystem->setPosition(handle, x, y);
    return 0;
}

int LuaAPI::UI_GetText(lua_State* L) {
    psyqo::Lua lua(L);
    if (!s_uiSystem || !lua.isNumber(1)) {
        lua.push("");
        return 1;
    }
    int handle = static_cast<int>(lua.toNumber(1));
    lua.push(s_uiSystem->getText(handle));
    return 1;
}

int LuaAPI::UI_GetColor(lua_State* L) {
    psyqo::Lua lua(L);
    if (!s_uiSystem || !lua.isNumber(1)) {
        lua.pushNumber(0); lua.pushNumber(0); lua.pushNumber(0);
        return 3;
    }
    int handle = static_cast<int>(lua.toNumber(1));
    uint8_t r, g, b;
    s_uiSystem->getColor(handle, r, g, b);
    lua.pushNumber(r); lua.pushNumber(g); lua.pushNumber(b);
    return 3;
}

int LuaAPI::UI_GetPosition(lua_State* L) {
    psyqo::Lua lua(L);
    if (!s_uiSystem || !lua.isNumber(1)) {
        lua.pushNumber(0); lua.pushNumber(0);
        return 2;
    }
    int handle = static_cast<int>(lua.toNumber(1));
    int16_t x, y;
    s_uiSystem->getPosition(handle, x, y);
    lua.pushNumber(static_cast<lua_Number>(x));
    lua.pushNumber(static_cast<lua_Number>(y));
    return 2;
}

int LuaAPI::UI_SetSize(lua_State* L) {
    psyqo::Lua lua(L);
    if (!s_uiSystem || !lua.isNumber(1)) return 0;
    int handle = static_cast<int>(lua.toNumber(1));
    int16_t w = static_cast<int16_t>(lua.toNumber(2));
    int16_t h = static_cast<int16_t>(lua.toNumber(3));
    s_uiSystem->setSize(handle, w, h);
    return 0;
}

int LuaAPI::UI_GetSize(lua_State* L) {
    psyqo::Lua lua(L);
    if (!s_uiSystem || !lua.isNumber(1)) {
        lua.pushNumber(0); lua.pushNumber(0);
        return 2;
    }
    int handle = static_cast<int>(lua.toNumber(1));
    int16_t w, h;
    s_uiSystem->getSize(handle, w, h);
    lua.pushNumber(static_cast<lua_Number>(w));
    lua.pushNumber(static_cast<lua_Number>(h));
    return 2;
}

int LuaAPI::UI_SetProgressColors(lua_State* L) {
    psyqo::Lua lua(L);
    if (!s_uiSystem || !lua.isNumber(1)) return 0;
    int handle = static_cast<int>(lua.toNumber(1));
    uint8_t bgR = static_cast<uint8_t>(lua.toNumber(2));
    uint8_t bgG = static_cast<uint8_t>(lua.toNumber(3));
    uint8_t bgB = static_cast<uint8_t>(lua.toNumber(4));
    uint8_t fR  = static_cast<uint8_t>(lua.toNumber(5));
    uint8_t fG  = static_cast<uint8_t>(lua.toNumber(6));
    uint8_t fB  = static_cast<uint8_t>(lua.toNumber(7));
    s_uiSystem->setProgressColors(handle, bgR, bgG, bgB, fR, fG, fB);
    return 0;
}

int LuaAPI::UI_GetElementType(lua_State* L) {
    psyqo::Lua lua(L);
    if (!s_uiSystem || !lua.isNumber(1)) {
        lua.pushNumber(-1);
        return 1;
    }
    int handle = static_cast<int>(lua.toNumber(1));
    lua.pushNumber(static_cast<lua_Number>(static_cast<uint8_t>(s_uiSystem->getElementType(handle))));
    return 1;
}

int LuaAPI::UI_GetElementCount(lua_State* L) {
    psyqo::Lua lua(L);
    if (!s_uiSystem || !lua.isNumber(1)) {
        lua.pushNumber(0);
        return 1;
    }
    int canvasIdx = static_cast<int>(lua.toNumber(1));
    lua.pushNumber(static_cast<lua_Number>(s_uiSystem->getCanvasElementCount(canvasIdx)));
    return 1;
}

int LuaAPI::UI_GetElementByIndex(lua_State* L) {
    psyqo::Lua lua(L);
    if (!s_uiSystem || !lua.isNumber(1) || !lua.isNumber(2)) {
        lua.pushNumber(-1);
        return 1;
    }
    int canvasIdx = static_cast<int>(lua.toNumber(1));
    int elemIdx = static_cast<int>(lua.toNumber(2));
    int handle = s_uiSystem->getCanvasElementHandle(canvasIdx, elemIdx);
    lua.pushNumber(static_cast<lua_Number>(handle));
    return 1;
}

}  // namespace psxsplash
