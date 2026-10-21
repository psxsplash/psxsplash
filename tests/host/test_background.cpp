// Host tests for the baked background table (src/backgroundmath.cpp).
//
// The table is written by SplashEdit and read in place on the console, so the
// layout is the contract: these tests pin the field offsets and walk a table
// built here byte by byte, the way an exported pack lays it out.

#include <stddef.h>
#include <string.h>

#include <vector>

#include "backgroundmath.hh"

#include "testing.hh"

using namespace psxsplash;

namespace {

struct Pack {
    std::vector<uint8_t> bytes;
    uint32_t put(const void* p, size_t n) {
        while (bytes.size() % 4) bytes.push_back(0);
        uint32_t at = (uint32_t)bytes.size();
        const uint8_t* b = static_cast<const uint8_t*>(p);
        bytes.insert(bytes.end(), b, b + n);
        return at;
    }
};

// A pack whose table holds "hall" (no start flag) and "yard" (shown at start),
// starting at a non-zero offset like a real one does.
uint32_t buildTable(Pack& pk) {
    const uint8_t filler[40] = {};
    pk.put(filler, sizeof(filler));
    const uint32_t hallName = pk.put("hall", 5);
    const uint32_t yardName = pk.put("yard", 5);
    BackgroundTile tiles[2] = {};
    tiles[0].w = 256;
    tiles[0].h = 240;
    tiles[1].x = 256;
    tiles[1].w = 64;
    tiles[1].h = 240;
    const uint32_t tilesAt = pk.put(tiles, sizeof(tiles));
    BackgroundTile layer[2] = {};
    layer[0].x = 32, layer[0].w = layer[0].h = 16, layer[0].nearSZ = 300;
    layer[1].x = 48, layer[1].w = layer[1].h = 16, layer[1].nearSZ = 420;
    const uint32_t layerAt = pk.put(layer, sizeof(layer));
    const uint16_t hidden[3] = {0, 4, 7};
    const uint32_t hiddenAt = pk.put(hidden, sizeof(hidden));

    BackgroundTableHeader th = {2, 0};
    BackgroundRecord recs[2] = {};
    recs[0].nameOffset = hallName;
    recs[0].h = 120;
    recs[1].nameOffset = yardName;
    recs[1].flags = BACKGROUND_FLAG_SHOW_AT_START;
    recs[1].tileCount = 2;
    recs[1].tilesOffset = tilesAt;
    recs[1].layerTileCount = 2;
    recs[1].layerTilesOffset = layerAt;
    recs[1].hiddenCount = 3;
    recs[1].hiddenOffset = hiddenAt;
    const uint32_t table = pk.put(&th, sizeof(th));
    pk.put(recs, sizeof(recs));
    return table;
}

}  // namespace

TEST(record_layout_is_the_contract) {
    CHECK_EQ(offsetof(BackgroundRecord, camX), 4);
    CHECK_EQ(offsetof(BackgroundRecord, rot), 16);
    CHECK_EQ(offsetof(BackgroundRecord, angleX), 34);
    CHECK_EQ(offsetof(BackgroundRecord, h), 40);
    CHECK_EQ(offsetof(BackgroundRecord, tileCount), 42);
    CHECK_EQ(offsetof(BackgroundRecord, layerTileCount), 44);
    CHECK_EQ(offsetof(BackgroundRecord, hiddenCount), 46);
    CHECK_EQ(offsetof(BackgroundRecord, flags), 48);
    CHECK_EQ(offsetof(BackgroundRecord, tilesOffset), 52);
    CHECK_EQ(offsetof(BackgroundRecord, layerTilesOffset), 56);
    CHECK_EQ(offsetof(BackgroundRecord, hiddenOffset), 60);
    CHECK_EQ(offsetof(BackgroundTile, w), 4);
    CHECK_EQ(offsetof(BackgroundTile, clutX), 8);
    CHECK_EQ(offsetof(BackgroundTile, texpageX), 12);
    CHECK_EQ(offsetof(BackgroundTile, u), 14);
    CHECK_EQ(offsetof(BackgroundTile, bitDepth), 16);
    CHECK_EQ(offsetof(BackgroundTile, nearSZ), 18);
}

TEST(no_table_reads_as_none) {
    const uint8_t pack[8] = {};
    CHECK_EQ(backgroundmath::count(pack, 0), 0);
    CHECK(backgroundmath::record(pack, 0, 0) == nullptr);
    CHECK_EQ(backgroundmath::find(pack, 0, "yard"), BACKGROUND_NONE);
    CHECK_EQ(backgroundmath::startIndex(pack, 0), BACKGROUND_NONE);
    CHECK_EQ(backgroundmath::count(nullptr, 12), 0);
}

TEST(records_by_index_and_name) {
    Pack pk;
    const uint32_t table = buildTable(pk);
    const uint8_t* p = pk.bytes.data();
    CHECK_EQ(backgroundmath::count(p, table), 2);
    CHECK_EQ(backgroundmath::find(p, table, "hall"), 0);
    CHECK_EQ(backgroundmath::find(p, table, "yard"), 1);
    CHECK_EQ(backgroundmath::find(p, table, "yar"), BACKGROUND_NONE);
    CHECK_EQ(backgroundmath::find(p, table, "yards"), BACKGROUND_NONE);
    CHECK_EQ(backgroundmath::find(p, table, nullptr), BACKGROUND_NONE);
    CHECK(backgroundmath::record(p, table, 2) == nullptr);
    CHECK(backgroundmath::record(p, table, -1) == nullptr);
    CHECK_EQ(backgroundmath::record(p, table, 0)->h, 120);
}

TEST(start_flag_picks_the_first_flagged) {
    Pack pk;
    const uint32_t table = buildTable(pk);
    CHECK_EQ(backgroundmath::startIndex(pk.bytes.data(), table), 1);
}

TEST(tiles_layer_tiles_and_hidden_list_follow_their_offsets) {
    Pack pk;
    const uint32_t table = buildTable(pk);
    const uint8_t* p = pk.bytes.data();
    const BackgroundRecord* r = backgroundmath::record(p, table, 1);
    const BackgroundTile* t = reinterpret_cast<const BackgroundTile*>(p + r->tilesOffset);
    CHECK_EQ(t[0].w + t[1].w, 320);
    CHECK_EQ(t[1].x, 256);
    const BackgroundTile* l = reinterpret_cast<const BackgroundTile*>(p + r->layerTilesOffset);
    CHECK_EQ(l[0].nearSZ, 300);
    CHECK_EQ(l[1].nearSZ, 420);
    CHECK_EQ(l[1].x, 48);
    const uint16_t* h = reinterpret_cast<const uint16_t*>(p + r->hiddenOffset);
    CHECK_EQ(h[0] + h[1] + h[2], 11);
}

TEST(image_bucket_is_in_front_of_the_dither_page) {
    // The renderer puts the dither TPage at otSize - 1; the image must draw after it.
    CHECK_EQ(backgroundmath::imageBucket(2048), 2046);
    CHECK_EQ(backgroundmath::imageBucket(1), 0);
}

TEST(layer_tile_bucket_is_its_near_depth_clamped_to_the_world_band) {
    CHECK_EQ(backgroundmath::layerBucket(300, 2048, 14), 300);
    CHECK_EQ(backgroundmath::layerBucket(3, 2048, 14), 14);
    CHECK_EQ(backgroundmath::layerBucket(5000, 2048, 14), 2046);
    // A triangle whose farthest corner is past the layer's bucket sorts behind it.
    const int32_t layer = backgroundmath::layerBucket(300, 2048, 14);
    CHECK(301 > layer);
}

int main() { return psxsplash::test::runAll(); }
