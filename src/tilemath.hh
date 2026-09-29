#pragma once

#include <stdint.h>

// The tilemap data model and the arithmetic over it, deliberately free of psyqo,
// the GPU and every global - the same split spritemath.hh makes from
// spritesystem.hh. That is what lets tests/host/test_tile.cpp compile this exact
// code natively and run it without a PlayStation.
//
// Everything that needs hardware (rendering, the splashpack pointers) lives in
// tilesystem.hh, which includes this.

namespace psxsplash {

/// One map cell. Kept to 2 bytes so a large map costs little: a 64x64 map is
/// 8 KB. The two concerns are kept separate on purpose - `tile` is *what to
/// draw*, `flags` is *how the world behaves here* - because a wall and a floor
/// can share a graphic and a floor and a task can share walkability.
struct TileCell {
    uint8_t tile;   ///< tileset cell index to draw; TILE_EMPTY = draw nothing
    uint8_t flags;  ///< bit0 = walkable (see TILE_FLAG_*)
};

/// An addressable object lifted out of the map at export time, so neither the
/// engine nor Lua ever scans the whole grid to find one. `kind` and `id` are
/// OPAQUE bytes the engine never interprets - a game decides what "kind 1, id 3"
/// means. The engine only stores them and hands them back through Tile.ObjectAt.
/// This is what keeps the tilemap game-agnostic: floors, walls and "some tagged
/// object here" are universal; "task" versus "vent" is a game's business.
struct TileObject {
    uint8_t kind;    ///< opaque game-defined category (0 is conventionally "none")
    uint8_t id;      ///< opaque game-defined index within the kind
    uint16_t tileX;  ///< grid coordinates of the cell it sits on
    uint16_t tileY;
};

/// Empty cell sentinel: nothing is drawn and (with the walkable bit clear) the
/// cell is solid, so unpainted space reads as void the player cannot enter.
static constexpr uint8_t TILE_EMPTY = 0xFF;

static constexpr uint8_t TILE_FLAG_WALKABLE = 0x01;

namespace tilemath {

/// Floor-divide a pixel coordinate to a tile coordinate, correct for negatives.
/// Plain C division truncates toward zero, which folds pixel -1 and +1 both onto
/// tile 0 and makes the row of tiles left of the origin one pixel too wide. A
/// zero tile size is treated as 1 so a malformed map degrades instead of
/// dividing by zero.
int32_t pixelToTile(int32_t px, uint8_t tileSize);

/// Linear index of (tileX, tileY) into a width*height cell array, or -1 when the
/// coordinate is off the map.
int cellIndex(int32_t tileX, int32_t tileY, uint16_t width, uint16_t height);

/// Is the map walkable at this pixel? Off the map is NOT walkable, so the map's
/// edge is a wall for free and a player can never wander into unpainted space.
/// A null cell array is treated as "no map", which is likewise not walkable.
bool walkableAtPixel(const TileCell* cells, uint16_t width, uint16_t height,
                     uint8_t tileW, uint8_t tileH, int32_t px, int32_t pz);

/// Is there an unobstructed straight line between two pixels?
///
/// Walks the segment sampling the map, and reports false the moment it crosses a
/// cell that is not walkable. Games use this for what a character can SEE, which
/// is why it lives beside the walkability test rather than in a game: it is the
/// same question the collision code asks, along a line instead of at a point.
///
/// The sample stride is half the smaller tile dimension, and that bound is what
/// makes the result trustworthy rather than approximate: an obstacle is at least
/// one tile across, so sampling at least twice per tile cannot step over one.
/// Doing this in Lua instead would mean a `Tile.Walkable` call per sample -
/// hundreds per frame, each one a full Lua-to-C++ transition, on a 33MHz CPU.
///
/// `maxSteps` bounds the work: a segment needing more samples than that reports
/// false (nothing that far away is being drawn anyway). A null cell array means
/// "no map", which is treated as nothing to block the view - the same
/// degradation `walkableAtPixel`'s callers get, so a mapless scene keeps working.
bool sightClear(const TileCell* cells, uint16_t width, uint16_t height, uint8_t tileW,
                uint8_t tileH, int32_t x0, int32_t z0, int32_t x1, int32_t z1,
                uint16_t maxSteps);

/// Centre pixel of a tile, the natural spawn/anchor point for anything that
/// lives on a cell (an avatar standing on a spawn tile, a sprite drawn for a
/// task station). Writes both axes; either output pointer may be null.
void tileCenterPixel(uint16_t tileX, uint16_t tileY, uint8_t tileW, uint8_t tileH,
                     int32_t* px, int32_t* pz);

}  // namespace tilemath
}  // namespace psxsplash
