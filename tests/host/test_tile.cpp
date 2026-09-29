// Host tests for the tilemap math (src/tilemath.cpp), compiled natively.
//
// Rendering a tilemap needs a PlayStation. Deciding which tile a pixel is in,
// whether that tile is a wall, and where a tile's centre lands does not — and
// that arithmetic is exactly where the bugs are: the sign of a floor-divide, an
// off-by-one at the map edge, a walkable test that reads out of bounds. All of
// it is checkable here instead of by walking a character into a wall on hardware.

#include "tilemath.hh"

#include "testing.hh"

using namespace psxsplash;
using namespace psxsplash::tilemath;

namespace {

// A 4x3 map of 16-px tiles. Row 1 (the middle row) is an open corridor; every
// other cell is a solid wall. Cell (2,1) is a task, so it is walkable AND
// addressable.
//   . . . .
//   # # T #    <- y=1 walkable, (2,1) is the task
//   . . . .
TileCell makeMap(TileCell (&cells)[12]) {
    for (int i = 0; i < 12; i++) cells[i] = {TILE_EMPTY, 0};
    // walkable corridor across y == 1
    for (int x = 0; x < 4; x++) cells[1 * 4 + x] = {(uint8_t)x, TILE_FLAG_WALKABLE};
    return cells[0];
}

}  // namespace

// ---------------------------------------------------------------------------
// pixelToTile
// ---------------------------------------------------------------------------

TEST(pixel_to_tile_origin_and_first_cell) {
    CHECK_EQ(pixelToTile(0, 16), 0);
    CHECK_EQ(pixelToTile(15, 16), 0);
    CHECK_EQ(pixelToTile(16, 16), 1);
    CHECK_EQ(pixelToTile(31, 16), 1);
    CHECK_EQ(pixelToTile(32, 16), 2);
}

TEST(pixel_to_tile_floors_toward_negative_infinity) {
    // The bug this guards: C division truncates toward zero, so a naive px/size
    // maps -1 and +1 both onto tile 0 and the leftmost negative column is wrong.
    CHECK_EQ(pixelToTile(-1, 16), -1);
    CHECK_EQ(pixelToTile(-16, 16), -1);
    CHECK_EQ(pixelToTile(-17, 16), -2);
    CHECK_EQ(pixelToTile(-32, 16), -2);
}

TEST(pixel_to_tile_zero_size_does_not_divide_by_zero) {
    CHECK_EQ(pixelToTile(100, 0), 100);  // degrades to one pixel per tile
}

TEST(pixel_to_tile_handles_larger_tiles) {
    CHECK_EQ(pixelToTile(0, 32), 0);
    CHECK_EQ(pixelToTile(31, 32), 0);
    CHECK_EQ(pixelToTile(32, 32), 1);
    CHECK_EQ(pixelToTile(-1, 32), -1);
}

// ---------------------------------------------------------------------------
// cellIndex
// ---------------------------------------------------------------------------

TEST(cell_index_row_major_inside_the_map) {
    CHECK_EQ(cellIndex(0, 0, 4, 3), 0);
    CHECK_EQ(cellIndex(3, 0, 4, 3), 3);
    CHECK_EQ(cellIndex(0, 1, 4, 3), 4);
    CHECK_EQ(cellIndex(3, 2, 4, 3), 11);
}

TEST(cell_index_off_the_map_is_minus_one) {
    CHECK_EQ(cellIndex(-1, 0, 4, 3), -1);
    CHECK_EQ(cellIndex(0, -1, 4, 3), -1);
    CHECK_EQ(cellIndex(4, 0, 4, 3), -1);  // x == width
    CHECK_EQ(cellIndex(0, 3, 4, 3), -1);  // y == height
}

// ---------------------------------------------------------------------------
// walkableAtPixel
// ---------------------------------------------------------------------------

TEST(walkable_true_on_the_corridor) {
    TileCell cells[12];
    makeMap(cells);
    // Anywhere inside the y==1 row (pixels 16..31) is walkable.
    CHECK(walkableAtPixel(cells, 4, 3, 16, 16, 0, 16));
    CHECK(walkableAtPixel(cells, 4, 3, 16, 16, 40, 24));
    CHECK(walkableAtPixel(cells, 4, 3, 16, 16, 63, 31));
}

TEST(walkable_false_on_a_wall) {
    TileCell cells[12];
    makeMap(cells);
    CHECK(!walkableAtPixel(cells, 4, 3, 16, 16, 0, 0));    // top-left wall
    CHECK(!walkableAtPixel(cells, 4, 3, 16, 16, 40, 40));  // bottom row wall
}

TEST(walkable_false_off_the_map_edge) {
    TileCell cells[12];
    makeMap(cells);
    CHECK(!walkableAtPixel(cells, 4, 3, 16, 16, -1, 16));   // left of the map
    CHECK(!walkableAtPixel(cells, 4, 3, 16, 16, 64, 16));   // right of the map
    CHECK(!walkableAtPixel(cells, 4, 3, 16, 16, 16, -1));   // above the map
    CHECK(!walkableAtPixel(cells, 4, 3, 16, 16, 16, 48));   // below the map
}

TEST(walkable_false_for_a_null_map) {
    CHECK(!walkableAtPixel(nullptr, 4, 3, 16, 16, 16, 16));
}

// ---------------------------------------------------------------------------
// tileCenterPixel
// ---------------------------------------------------------------------------

TEST(tile_center_is_offset_by_half_a_tile) {
    int32_t px = -1, pz = -1;
    tileCenterPixel(0, 0, 16, 16, &px, &pz);
    CHECK_EQ(px, 8);
    CHECK_EQ(pz, 8);

    tileCenterPixel(2, 1, 16, 16, &px, &pz);
    CHECK_EQ(px, 2 * 16 + 8);
    CHECK_EQ(pz, 1 * 16 + 8);
}

TEST(tile_center_tolerates_null_outputs) {
    tileCenterPixel(1, 1, 16, 16, nullptr, nullptr);  // must not crash
    CHECK(true);
}

// ---------------------------------------------------------------------------
// sightClear
//
// This is what hides a player behind a wall, so the failures matter in opposite
// directions: report clear through a wall and the whole mechanic is cosmetic;
// report blocked along an open corridor and players wink out in plain sight.
// ---------------------------------------------------------------------------

namespace {

// An 8x5 map of 16-px tiles: two open rooms joined by a one-tile doorway, with
// a solid pillar between them. This is the shape the maps are actually built
// from, and the one where a naive "sample every tile centre" ray sees through
// the pillar.
//   # # # # # # # #
//   # . . . # . . #
//   # . . . D . . #     D = the doorway at (4,2)
//   # . . . # . . #
//   # # # # # # # #
TileCell makeRooms(TileCell (&cells)[40]) {
    for (int i = 0; i < 40; i++) cells[i] = {TILE_EMPTY, 0};
    auto open = [&](int x, int y) { cells[y * 8 + x] = {1, TILE_FLAG_WALKABLE}; };
    for (int y = 1; y <= 3; y++) {
        for (int x = 1; x <= 3; x++) open(x, y);
        for (int x = 5; x <= 6; x++) open(x, y);
    }
    open(4, 2);  // the doorway
    return cells[0];
}

// Centre of tile (tx, ty) on a 16-px grid.
constexpr int32_t C(int t) { return t * 16 + 8; }

}  // namespace

TEST(sight_is_clear_along_an_open_row) {
    TileCell cells[40];
    makeRooms(cells);
    CHECK(sightClear(cells, 8, 5, 16, 16, C(1), C(1), C(3), C(1), 128));
}

TEST(sight_is_blocked_by_the_wall_between_the_rooms) {
    TileCell cells[40];
    makeRooms(cells);
    // Straight across row 1, which the pillar at (4,1) sits in.
    CHECK(!sightClear(cells, 8, 5, 16, 16, C(1), C(1), C(6), C(1), 128));
}

TEST(sight_passes_through_the_doorway) {
    TileCell cells[40];
    makeRooms(cells);
    CHECK(sightClear(cells, 8, 5, 16, 16, C(1), C(2), C(6), C(2), 128));
}

TEST(sight_is_blocked_diagonally_beside_the_doorway) {
    TileCell cells[40];
    makeRooms(cells);
    // (1,1) -> (5,3) crosses the wall cell at (4,3), one below the doorway.
    CHECK(!sightClear(cells, 8, 5, 16, 16, C(1), C(1), C(5), C(3), 128));
}

TEST(sight_threads_a_doorway_on_a_diagonal) {
    TileCell cells[40];
    makeRooms(cells);
    // The counterpart, and the more important direction to get right: this line
    // really does pass through the doorway cell, so reporting it blocked would
    // make players vanish while standing in an open door.
    CHECK(sightClear(cells, 8, 5, 16, 16, C(1), C(1), C(6), C(3), 128));
}

TEST(sight_is_blocked_off_the_edge_of_the_map) {
    TileCell cells[40];
    makeRooms(cells);
    CHECK(!sightClear(cells, 8, 5, 16, 16, C(1), C(1), 500, C(1), 128));
}

TEST(sight_to_yourself_is_always_clear) {
    TileCell cells[40];
    makeRooms(cells);
    CHECK(sightClear(cells, 8, 5, 16, 16, C(1), C(1), C(1), C(1), 128));
}

TEST(sight_with_no_map_is_never_blocked) {
    // A scene without a tilemap must keep everyone visible rather than hiding
    // the entire cast behind walls that do not exist.
    CHECK(sightClear(nullptr, 8, 5, 16, 16, 0, 0, 500, 500, 128));
}

TEST(sight_refuses_a_segment_longer_than_the_step_budget) {
    TileCell cells[40];
    makeRooms(cells);
    // 5 samples of budget cannot cover a 2000px line; report blocked rather
    // than walking thousands of cells to answer a question about something far
    // off screen.
    CHECK(!sightClear(cells, 8, 5, 16, 16, C(1), C(1), 2000, C(1), 5));
}

TEST(sight_samples_at_least_twice_per_tile) {
    // The guarantee the stride is chosen for: a one-tile obstacle cannot be
    // stepped over. A wall exactly one cell wide, sampled along its axis.
    TileCell cells[40];
    for (int i = 0; i < 40; i++) cells[i] = {1, TILE_FLAG_WALKABLE};
    cells[2 * 8 + 4] = {TILE_EMPTY, 0};  // one solid cell in an open field
    CHECK(!sightClear(cells, 8, 5, 16, 16, C(0), C(2), C(7), C(2), 128));
    // ...and a line one row above it is unaffected.
    CHECK(sightClear(cells, 8, 5, 16, 16, C(0), C(1), C(7), C(1), 128));
}

int main() { return psxsplash::test::runAll(); }
