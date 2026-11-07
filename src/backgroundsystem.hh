#pragma once

#include <EASTL/vector.h>

#include "backgroundmath.hh"
#include "gameobject.hh"
#include "renderer.hh"

namespace psxsplash {

class Camera;

// Baked backgrounds of a scene (splashpack v29). At most one shows at a time:
// its image fills the screen behind the world, its layers cut in front of
// whatever is drawn behind their depth, and the objects baked into it are
// not drawn (they still collide, trigger and carry nav).
class BackgroundSystem {
  public:
    void init();
    void load(const uint8_t* pack, uint32_t tableOffset, eastl::vector<GameObject*>* objects);

    int count() const { return backgroundmath::count(m_pack, m_table); }
    int find(const char* name) const { return backgroundmath::find(m_pack, m_table, name); }
    int startIndex() const { return backgroundmath::startIndex(m_pack, m_table); }
    int current() const { return m_current; }
    const char* name(int i) const;

    /// Shows background `i`: sets the camera pose and H and hides its objects.
    /// Returns false when there is no such background.
    bool show(int i, Camera& camera);
    /// Back to the 3D scene: the objects draw again. Does not touch the camera.
    void hide();

    void renderOT(Renderer::OT& ot, Renderer::Balloc& balloc, uint32_t otSize);

  private:
    void setHidden(int i, bool hidden);
    void insertTile(const BackgroundTile& t, int32_t bucket, Renderer::OT& ot, Renderer::Balloc& balloc);

    const uint8_t* m_pack = nullptr;
    uint32_t m_table = 0;
    eastl::vector<GameObject*>* m_objects = nullptr;
    int m_current = BACKGROUND_NONE;
};

}  // namespace psxsplash
