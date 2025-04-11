#include "lua.h"

#include <psyqo/xprintf.h>

#include "gameobject.hh"
#include "psyqo-lua/lua.hh"

constexpr const char METATABLE_SCRIPT[] = R"(
    print("test")
    metatableForAllGameObjects = {
        __index = function(self, key)
            if key == "position" then
                local pos = rawget(self, key)
                if pos == nil then
                    pos = get_position(self.__cpp_ptr)
                    rawset(self, key, pos)
                end
                return pos
            end
            return rawget(self, key)
        end,

        __newindex = function(self, key, value)
            if key == "position" then
                -- Option 1: Directly update C++
                set_position(self.__cpp_ptr, value)
                -- Option 2: Also update local cache:
                rawset(self, key, value)
                return
            end
            rawset(self, key, value)
        end
    }
)";

// Lua helpers

int luaPrint(psyqo::Lua L) {
    int n = L.getTop();  // Get the number of arguments

    for (int i = 1; i <= n; i++) {
        if (i > 1) {
            printf("\t");  // Tab between arguments
        }

        // Check the type of the argument
        if (L.isString(i)) {
            printf("%s", L.toString(i));  // If it's a string, print it
        } else if (L.isNumber(i)) {
            printf("%g", L.toNumber(i));  // If it's a number, print it
        } else {
            // For other types, just print their type (you can expand this if needed)
            printf("[%s]", L.typeName(i));
        }
    }
    printf("\n");

    return 0;  // No return value
}

static int gameobjectSetPosition(lua_State* L) {
    psxsplash::GameObject* go = (psxsplash::GameObject*)lua_touserdata(L, 1);
    lua_newtable(L);
    lua_pushnumber(L, go->position.x.raw());
    lua_setfield(L, -2, "x");
    lua_pushnumber(L, go->position.y.raw());
    lua_setfield(L, -2, "y");
    lua_pushnumber(L, go->position.z.raw());
    lua_setfield(L, -2, "z");
    return 1;
}

static int gameobjectGetPosition(lua_State* L) {
    psxsplash::GameObject* go = (psxsplash::GameObject*)lua_touserdata(L, 1);

    lua_getfield(L, 2, "x");
    psyqo::FixedPoint<> x(lua_tonumber(L, -1), psyqo::FixedPoint<>::RAW);
    go->position.x = x;
    lua_pop(L, 1);
    lua_getfield(L, 2, "y");
    psyqo::FixedPoint<> y(lua_tonumber(L, -1), psyqo::FixedPoint<>::RAW);
    go->position.x = x;
    lua_pop(L, 1);
    lua_getfield(L, 2, "z");
    psyqo::FixedPoint<> z(lua_tonumber(L, -1), psyqo::FixedPoint<>::RAW);
    go->position.x = x;
    lua_pop(L, 1);

    return 0;
}

void psxsplash::Lua::Init() {
    L.push(luaPrint);
    L.setGlobal("print");

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
    // Create a new Lua table for the GameObject
    L.newTable();

    // Set the __cpp_ptr field to store the C++ pointer
    L.push(go);                   
    L.setField(-2, "__cpp_ptr");  

    // Set the metatable for the table
    L.getGlobal("metatableForAllGameObjects");
    if (L.isTable(-1)) {
        L.setMetatable(-2);  // Set the metatable for the table
    } else {
        printf("Warning: metatableForAllGameObjects not found\n");
        L.pop();  // Pop the invalid metatable
    }

    L.push(go);                   
    L.push(-2);
    L.rawSet(LUA_REGISTRYINDEX); 

    // Debugging: Confirm the GameObject was registered
    printf("GameObject registered in Lua registry: %p\n", go);

    L.pop();
}

void psxsplash::Lua::CallOnCollide(GameObject* self, GameObject* other) {
    L.getGlobal("onCollision");
    if (!L.isFunction(-1)) {
        printf("Lua function 'onCollide' not found\n");
        L.pop();
        return;
    }

    PushGameObject(self);
    PushGameObject(other);

    if (L.pcall(2, 0)) {
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
