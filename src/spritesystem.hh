#pragma once

#include <stdint.h>

#include <psyqo/gpu.hh>
#include <psyqo/primitives/common.hh>
#include <psyqo/primitives/quads.hh>
#include <psyqo/primitives/sprites.hh>

#include "renderer.hh"
#include "spritemath.hh"

namespace psxsplash {

class GameObject;

static constexpr int SPRITE_MAX        = 128;
static constexpr int SPRITE_MAX_SHEETS = 16;
static constexpr int SPRITE_MAX_ANIMS  = 64;

/// Which space a sprite's position is in.
enum class SpritePlane : uint8_t {
    /// x/y are screen pixels, shifted by the global view offset. This is the 2D
    /// game case and the only one currently rendered.
    Screen = 0,
    /// x/y/z are world units, projected through the GTE and depth-sorted against
    /// 3D geometry. Reserved: carried through the data model but not yet drawn.
    World = 1,
};

struct SpriteInstance {
    bool active;
    bool visible;
    SpritePlane plane;
    uint8_t layer;  // 0 = frontmost sprite layer; clamped to SPRITE_LAYERS-1

    int16_t x, y;             // Screen plane: pixels
    psyqo::Vec3 worldPos;     // World plane
    int16_t w, h;             // 0 = use the sheet's cell size (the 1:1 fast path)

    uint8_t sheet;
    uint8_t frame;  // absolute cell index within the sheet
    bool flipX, flipY;
    psyqo::Color color;      // texture tint; 128/128/128 is neutral
    bool ignoreViewOffset;   // pin to the screen (HUD-like) despite the camera

    int8_t anim;  // index into m_anims, or -1
    uint8_t animFrame;  // index within the animation, not the sheet
    uint8_t animTimer;
    /// Sub-vsync-frame carry, in raw dt units (4096 == one 30Hz frame). Keeps
    /// animation speed independent of the frame rate; see SpriteSystem::update.
    int16_t animAccum;
    bool animPlaying;

    GameObject* boundActor;  // follow this actor's position, or null
    int16_t boundOffsetX, boundOffsetY;
};

/// Screen-space 2D sprites, drawn between the 3D scene and the UI.
///
/// Presentation only: a sprite bound to an actor reads that actor's position and
/// never writes it. Nothing here touches networking - a bound sprite follows a
/// replicated actor for free, because the actor is what replicates.
class SpriteSystem {
  public:
    void init();

    /// Parse sheet and animation tables. Called from SplashPackLoader (v22+).
    void loadFromSplashpack(uint8_t* data, uint16_t sheetCount, uint16_t animCount,
                            uint32_t tableOffset);

    void relocate(intptr_t delta);

    /// Advance animations and follow bound actors. Once per frame, before render.
    void update(int32_t dt12);

    /// Insert this frame's primitives. Called from the renderer before the UI's,
    /// so the UI stays on top.
    void renderOT(psyqo::OrderingTable<Renderer::ORDERING_TABLE_SIZE>& ot,
                  psyqo::BumpAllocator<Renderer::BUMP_ALLOCATOR_SIZE>& balloc);

    int sheetIndex(const char* name) const;
    int animIndex(const char* name) const;

    /// Claim a pooled sprite. Returns a handle, or -1 when the pool is full.
    int create(int sheet);
    void destroy(int id);

    void setPos(int id, int16_t x, int16_t y);
    void setWorldPos(int id, const psyqo::Vec3& p);
    void bindToActor(int id, GameObject* actor, SpritePlane plane, int16_t offX, int16_t offY);
    void setFrame(int id, uint8_t frame);
    void playAnim(int id, int anim, bool restart);
    void stopAnim(int id);
    /// Select a directional animation from `animBase` by yaw, keeping the current
    /// frame so a turning sprite does not stutter back to frame 0.
    void setFacingFromYaw(int id, int animBase, uint8_t dirCount, int32_t yawRaw);
    void setVisible(int id, bool v);
    bool isVisible(int id) const;
    void setFlip(int id, bool fx, bool fy);
    void setColor(int id, uint8_t r, uint8_t g, uint8_t b);
    void setLayer(int id, uint8_t layer);
    void setSize(int id, int16_t w, int16_t h);
    void setIgnoreViewOffset(int id, bool ignore);

    /// Scrolls every non-pinned screen-space sprite: a 2D camera, for free.
    void setViewOffset(int16_t x, int16_t y);
    void getViewOffset(int16_t& x, int16_t& y) const;

    int spriteCount() const { return m_spriteCount; }
    int sheetCount() const { return m_sheetCount; }
    int animCount() const { return m_animCount; }
    const SpriteAnim* anim(int i) const;
    /// Resolved atlas coordinates for a sheet, or null if out of range. The tile
    /// system uses this to draw a tilemap from a sheet it does not itself own.
    const SpriteSheet* sheet(int i) const;

  private:
    bool valid(int id) const { return id >= 0 && id < SPRITE_MAX && m_sprites[id].active; }

    void renderSprite(SpriteInstance& s, int depth,
                      psyqo::OrderingTable<Renderer::ORDERING_TABLE_SIZE>& ot,
                      psyqo::BumpAllocator<Renderer::BUMP_ALLOCATOR_SIZE>& balloc);

    static psyqo::PrimPieces::TPageAttr makeTPage(const SpriteSheet& sheet);

    SpriteSheet m_sheets[SPRITE_MAX_SHEETS];
    SpriteAnim m_anims[SPRITE_MAX_ANIMS];
    SpriteInstance m_sprites[SPRITE_MAX];
    int m_sheetCount = 0;
    int m_animCount = 0;
    int m_spriteCount = 0;  // live sprites, for diagnostics

    int16_t m_viewX = 0, m_viewY = 0;
};

}  // namespace psxsplash
