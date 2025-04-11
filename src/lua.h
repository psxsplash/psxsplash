#pragma once

#include <psyqo-lua/lua.hh>

#include "gameobject.hh"

namespace psxsplash {
class Lua {
  public:
    void Init();
    
    void LoadLuaFile(const char* code, size_t len);
    void RegisterGameObject(GameObject* go);
    void CallOnCollide(GameObject* self, GameObject* other);

  private:
    void PushGameObject(GameObject* go);
    psyqo::Lua L;
};
}  // namespace psxsplash