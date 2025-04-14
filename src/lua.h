#pragma once

#include <psyqo-lua/lua.hh>

#include "gameobject.hh"

namespace psxsplash {

struct LuaFile {
    union {
        uint32_t luaCodeOffset;
        const char* luaCode;
    };
    uint32_t length;
};

class Lua {
  public:
    void Init();

    void LoadLuaFile(const char* code, size_t len, int index);
    void RegisterGameObject(GameObject* go);
    void CallOnCollide(GameObject* self, GameObject* other);

  private:
    void PushGameObject(GameObject* go);
    psyqo::Lua L;

    int m_metatableReference;
    int m_luascriptsReference;
};
}  // namespace psxsplash