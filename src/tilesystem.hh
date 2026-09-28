#pragma once

#include <stdint.h>

#include <psyqo/gpu.hh>
#include <psyqo/primitives/common.hh>
#include <psyqo/primitives/sprites.hh>

#include "renderer.hh"
#include "spritemath.hh"  // SpriteSheet + frameToUV, shared with the sprite path
#include "tilemath.hh"

namespace psxsplash {

class SpriteSystem;

/// Addressable-object pool. A map with more than this many task/vent/etc. tiles
/// is refused at export, so this only ever caps authoring, never the runtime.
static constexpr int TILE_MAX_OBJECTS = 128;

/// A screen-space tilemap: one grid of cells drawn from a tileset sheet, plus
/// the walkability of each cell and the addressable objects painted onto it.
///
/// It draws behind the sprites (the floor a character stands on) and scrolls
/// with the very same view offset the sprite system uses, so one camera moves
/// the whole 2D world. Like the sprite system it owns no networking and uploads
/// no pixels: the tileset already rides the shared VRAM atlas, and the cells and
/// objects point straight into the loaded splashpack.
class TileSystem {
  public:
    void init();

    /// Parse the tilemap chunk (v23+). `sprites` resolves the tileset sheet's
    /// atlas coordinates; the map is drawn with that sheet's page/CLUT/UV grid.
    void loadFromSplashpack(uint8_t* data, uint32_t tableOffset, SpriteSystem* sprites);

    /// True once a map is loaded. A scene with no tilemap leaves this false and
    /// every query below answers as if the world were empty (not walkable).
    bool active() const { return m_cells != nullptr; }

    // --- queries, all in world pixels with tile (0,0) at the origin ---

    /// Is the map walkable at this pixel? Off the map, or with no map at all, is
    /// NOT walkable - the edge of the painted world is a wall for free.
    bool walkableAtPixel(int32_t px, int32_t pz) const;

    /// Is the straight line between two pixels free of solid cells? See
    /// tilemath::sightClear - this only supplies the map.
    bool sightClear(int32_t x0, int32_t z0, int32_t x1, int32_t z1,
                    uint16_t maxSteps = 128) const;

    int objectCount() const { return m_objectCount; }
    const TileObject* object(int i) const;
    /// Centre pixel of object `i`. Either output pointer may be null.
    void objectCenterPixel(int i, int32_t* px, int32_t* pz) const;

    uint16_t width() const { return m_width; }
    uint16_t height() const { return m_height; }
    uint8_t tileW() const { return m_tileW; }
    uint8_t tileH() const { return m_tileH; }

    /// Insert this frame's tile primitives. Called from the renderer BEFORE the
    /// sprite system's, so sprites land on top of the floor.
    void renderOT(psyqo::OrderingTable<Renderer::ORDERING_TABLE_SIZE>& ot,
                  psyqo::BumpAllocator<Renderer::BUMP_ALLOCATOR_SIZE>& balloc);

  private:
    psyqo::PrimPieces::TPageAttr makeTPage() const;

    const TileCell* m_cells = nullptr;      // points into splashpack data
    const TileObject* m_objects = nullptr;  // points into splashpack data
    int m_objectCount = 0;
    uint16_t m_width = 0, m_height = 0;
    uint8_t m_tileW = 0, m_tileH = 0;

    SpriteSystem* m_sprites = nullptr;  // for the shared view offset
    SpriteSheet m_sheet{};              // resolved copy of the tileset sheet
    bool m_haveSheet = false;
};

}  // namespace psxsplash
