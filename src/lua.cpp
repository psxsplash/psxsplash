#include "lua.h"

#include <psyqo-lua/lua.hh>

#include <psyqo/xprintf.h>

#include "gameobject.hh"

constexpr const char METATABLE_SCRIPT[] = R"(
return function(metatable)
    local get_position = metatable.get_position
    local set_position = metatable.set_position

    metatable.get_position = nil
    metatable.set_position = nil

    function metatable.__index(self, key)
        if key == "position" then
            return get_position(self.__cpp_ptr)
        end
        return nil
    end

    function metatable.__newindex(self, key, value)
        if key == "position" then
            set_position(self.__cpp_ptr, value)
            return
        end
        rawset(self, key, value)
    end
end
)";

// Lua helpers

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
    // Load and run the metatable script
    if (L.loadBuffer(METATABLE_SCRIPT, "buffer:metatableForAllGameObjects") == 0) {
        if (L.pcall(0, 1) == 0) {
            // This will be our metatable
            L.newTable();

            L.push(gameobjectGetPosition);
            L.setField(-2, "get_position");

            L.push(gameobjectSetPosition);
            L.setField(-2, "set_position");

            L.copy(-1);
            m_metatableReference = L.ref();

            if (L.pcall(1, 0) == 0) {
                printf("Lua script 'metatableForAllGameObjects' executed successfully");
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
}

void psxsplash::Lua::LoadLuaFile(const char* code, size_t len, int index) {
    char filename[32];
    snprintf(filename, sizeof(filename), "lua_asset:%d", index);
    if (L.loadBuffer(code, len, filename) != LUA_OK) {
        printf("Lua error: %s\n", L.toString(-1));
        L.pop();
    }
    // (1) script func
    L.rawGetI(LUA_REGISTRYINDEX, m_luascriptsReference);
    // (1) script func (2) scripts table
    L.newTable();
    // (1) script func (2) scripts table (3) {}
    L.pushNumber(index);
    // (1) script func (2) scripts table (3) {} (4) index
    L.copy(-2);
    // (1) script func (2) scripts table (3) {} (4) index (5) {}
    L.setTable(-4);
    // (1) script func (2) scripts table (3) {}
    lua_setupvalue(L.getState(), -3, 1);
    // (1) script func (2) scripts table
    L.pop();
    // (1) script func
    if (L.pcall(0, 0)) {
        printf("Lua error: %s\n", L.toString(-1));
        L.pop();
    }
}

void psxsplash::Lua::RegisterGameObject(GameObject* go) {
    L.push(go);
    // (1) go
    L.newTable();
    // (1) go (2) {}
    L.push(go);
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
    printf("GameObject registered in Lua registry: %p\n", go);
}

void psxsplash::Lua::CallOnCollide(GameObject* self, GameObject* other) {
    if (self->luaFileIndex == -1) {
        return;
    }
    L.rawGetI(LUA_REGISTRYINDEX, m_luascriptsReference);
    // (1) scripts table
    L.rawGetI(-1, self->luaFileIndex);
    // (1) script table (2) script environment
    L.getField(-1, "onCollision");
    // (1) script table (2) script environment (3) onCollision
    if (!L.isFunction(-1)) {
        printf("Lua function 'onCollision' not found\n");
        L.clearStack();
        return;
    }

    PushGameObject(self);
    PushGameObject(other);

    if (L.pcall(2, 0) != LUA_OK) {
        printf("Lua error: %s\n", L.toString(-1));
    }
    L.clearStack();
}

void psxsplash::Lua::PushGameObject(GameObject* go) {
    L.push(go);
    L.rawGet(LUA_REGISTRYINDEX);

    if (!L.isTable(-1)) {
        printf("Warning: GameObject not found in Lua registry\n");
        L.pop();
    }
}
