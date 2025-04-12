#include "lua.h"

#include <psyqo/xprintf.h>

#include "gameobject.hh"
#include "psyqo-lua/lua.hh"

constexpr const char METATABLE_SCRIPT[] = R"(
    metatableForAllGameObjects = {
        __index = function(self, key)
            if key == "position" then
                local pos = rawget(self, key)
                if pos == nil then
                    pos = get_position(self.__cpp_ptr)
                end
                return pos
            end
            return nil
        end,

        __newindex = function(self, key, value)
            if key == "position" then
                set_position(self.__cpp_ptr, value)
                return
            end
            rawset(self, key, value)
        end
    }
)";

// Lua helpers

int traceback(lua_State *L) {
    const char *msg = lua_tostring(L, 1);
    if (msg)
        luaL_traceback(L, L, msg, 1);
    else
        lua_pushliteral(L, "(no error message)");
    return 1;
}

static int gameobjectSetPosition(psyqo::Lua L) {
    auto go = L.toUserdata<psxsplash::GameObject>(1);
    L.getField(2, "x");
    psyqo::FixedPoint<> x(L.toNumber(3), psyqo::FixedPoint<>::RAW);
    go->position.x = x;
    L.pop();
    L.getField(2, "y");
    psyqo::FixedPoint<> y(L.toNumber(3), psyqo::FixedPoint<>::RAW);
    go->position.y = y;
    L.pop();
    L.getField(2, "z");
    psyqo::FixedPoint<> z(L.toNumber(3), psyqo::FixedPoint<>::RAW);
    go->position.z = z;
    L.pop();
    return 0;
}

static int gameobjectGetPosition(psyqo::Lua L) {
    auto go = L.toUserdata<psxsplash::GameObject>(1);
    L.newTable();
    L.pushNumber(go->position.x.raw());
    L.setField(2, "x");
    L.pushNumber(go->position.y.raw());
    L.setField(2, "y");
    L.pushNumber(go->position.z.raw());
    L.setField(2, "z");
    return 1;
}

void psxsplash::Lua::Init() {
//    L.push(luaPrint);
//    L.setGlobal("print");

    L.push(gameobjectGetPosition);
    L.setGlobal("get_position");

    L.push(gameobjectSetPosition);
    L.setGlobal("set_position");

    // Load and run the metatable script
    if (L.loadBuffer(METATABLE_SCRIPT, "metatableForAllGameObjects") == 0) {
        if (L.pcall(0, 0) == 0) {
            // Script executed successfully
            printf("Lua script 'metatableForAllGameObjects' loaded successfully\n");
        } else {
            // Print Lua error if script execution fails
            printf("Error executing Lua script: %s\n", L.isString(-1) ? L.toString(-1) : "Unknown error");
            L.pop();
        }
    } else {
        // Print Lua error if script loading fails
        printf("Error loading Lua script: %s\n", L.isString(-1) ? L.toString(-1) : "Unknown error");
        L.pop();
    }

    // Check if the metatable was set as a global
    L.getGlobal("metatableForAllGameObjects");
    if (L.isTable(-1)) {
        printf("metatableForAllGameObjects successfully set as a global\n");
    } else {
        printf("Warning: metatableForAllGameObjects not found after init\n");
    }
    L.pop(); // Pop the global check
}

void psxsplash::Lua::LoadLuaFile(const char* code, size_t len) {
    if (L.loadBuffer(code, len) != LUA_OK) {
        printf("Lua error: %s\n", L.toString(-1));
        L.pop();
    }
    if (L.pcall(0, 0)) {
        printf("Lua error: %s\n", L.toString(-1));
        L.pop();
    }
}

void psxsplash::Lua::RegisterGameObject(GameObject* go) {
    L.push(go);     // (1) = GameObject*
    // Create a new Lua table for the GameObject
    L.newTable();   // (1) = GameObject*, (2) = {}

    // Set the __cpp_ptr field to store the C++ pointer
    L.push(go);     // (1) = GameObject*, (2) = {}, (3) = GameObject*
    L.setField(-2, "__cpp_ptr");
                    // (1) = GameObject*, (2) = { __cpp_ptr = GameObject* }

    // Set the metatable for the table
    L.getGlobal("metatableForAllGameObjects");
                    // (1) = GameObject*, (2) = { __cpp_ptr = GameObject* }, (3) = metatableForAllGameObjects
    if (L.isTable(-1)) {
        L.setMetatable(-2);  // Set the metatable for the table
    } else {
        printf("Warning: metatableForAllGameObjects not found\n");
        L.pop();  // Pop the invalid metatable
    }
                    // (1) = GameObject*, (2) = { __cpp_ptr = GameObject* + metatable }

    L.rawSet(LUA_REGISTRYINDEX);

                    // stack empty

    // Debugging: Confirm the GameObject was registered
    printf("GameObject registered in Lua registry: %p\n", go);
}

void psxsplash::Lua::CallOnCollide(GameObject* self, GameObject* other) {
    L.push(traceback);
    int errfunc = L.getTop();  // Save the error function index

    L.getGlobal("onCollision");
    if (!L.isFunction(-1)) {
        printf("Lua function 'onCollide' not found\n");
        L.pop();
        return;
    }

    PushGameObject(self);
    PushGameObject(other);

    if (L.pcall(2, 0) != LUA_OK) {
        printf("Lua error: %s\n", L.toString(-1));
        L.pop();
    }
}

void psxsplash::Lua::PushGameObject(GameObject* go) {
    L.push(go);         
    L.rawGet(LUA_REGISTRYINDEX);  

    if (!L.isTable(-1)) {         
        printf("Warning: GameObject not found in Lua registry\n");
        L.pop();                 
    }
}
