#include "tilemath.hh"

namespace psxsplash {
namespace tilemath {

int32_t pixelToTile(int32_t px, uint8_t tileSize) {
    const int32_t s = tileSize ? tileSize : 1;
    if (px >= 0) return px / s;
    // Round toward negative infinity: -1px must land on tile -1, not tile 0.
    return -(((-px) + s - 1) / s);
}

int cellIndex(int32_t tileX, int32_t tileY, uint16_t width, uint16_t height) {
    if (tileX < 0 || tileY < 0 || tileX >= (int32_t)width || tileY >= (int32_t)height) return -1;
    return tileY * (int)width + tileX;
}

bool walkableAtPixel(const TileCell* cells, uint16_t width, uint16_t height, uint8_t tileW,
                     uint8_t tileH, int32_t px, int32_t pz) {
    if (!cells) return false;
    const int32_t tx = pixelToTile(px, tileW);
    const int32_t ty = pixelToTile(pz, tileH);
    const int idx = cellIndex(tx, ty, width, height);
    if (idx < 0) return false;
    return (cells[idx].flags & TILE_FLAG_WALKABLE) != 0;
}

bool sightClear(const TileCell* cells, uint16_t width, uint16_t height, uint8_t tileW,
                uint8_t tileH, int32_t x0, int32_t z0, int32_t x1, int32_t z1,
                uint16_t maxSteps) {
    // No map: nothing can block a view that has no walls in it.
    if (!cells) return true;

    const int32_t dx = x1 - x0;
    const int32_t dz = z1 - z0;
    const int32_t adx = dx >= 0 ? dx : -dx;
    const int32_t adz = dz >= 0 ? dz : -dz;
    const int32_t span = adx > adz ? adx : adz;
    if (span == 0) return true;  // same pixel; you can always see yourself

    // Half the smaller tile, so two samples land inside any one-tile obstacle.
    const int32_t tw = tileW ? tileW : 1;
    const int32_t th = tileH ? tileH : 1;
    int32_t stride = (tw < th ? tw : th) / 2;
    if (stride < 1) stride = 1;

    int32_t steps = span / stride;
    if (steps < 1) steps = 1;
    if (steps > (int32_t)maxSteps) return false;

    // i starts at 1: the caller's own cell is where they are standing, and a
    // character standing in a doorway must not block their own view. The
    // endpoint IS sampled, which is deliberate - a target inside a wall is not
    // visible even if every cell on the way to it is clear.
    for (int32_t i = 1; i <= steps; ++i) {
        // dx * i stays well inside 32 bits: a map is at most 256 tiles of 255
        // pixels, and steps is bounded by maxSteps.
        const int32_t px = x0 + (dx * i) / steps;
        const int32_t pz = z0 + (dz * i) / steps;
        if (!walkableAtPixel(cells, width, height, tileW, tileH, px, pz)) return false;
    }
    return true;
}

void tileCenterPixel(uint16_t tileX, uint16_t tileY, uint8_t tileW, uint8_t tileH, int32_t* px,
                     int32_t* pz) {
    if (px) *px = (int32_t)tileX * tileW + tileW / 2;
    if (pz) *pz = (int32_t)tileY * tileH + tileH / 2;
}

}  // namespace tilemath
}  // namespace psxsplash
