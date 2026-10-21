#pragma once

#include <stdint.h>

// Baked backgrounds (splashpack v28): the records as stored, and the reads
// over them, free of psyqo so tests/host/test_background.cpp can run this exact
// code natively. Drawing them lives in backgroundsystem.cpp.
//
// A background is a fixed camera's view rendered offline by the editor: a
// full-screen image drawn behind every 3D triangle, and depth layers cut from
// the same render. Each layer is a set of cutout tiles drawn at one ordering
// table bucket, the nearest depth of its pixels, so a character whose farthest
// vertex is beyond that bucket draws behind the layer.

namespace psxsplash {

/// One screen-aligned piece of a background or a layer, drawn as a sprite.
/// w and h are at most 256 (one texture page across).
struct BackgroundTile {
    int16_t x, y;            ///< screen position of the top-left texel
    uint16_t w, h;
    uint16_t clutX, clutY;   ///< clutX in 16-word units; 0,0 for 16 bpp
    uint8_t texpageX, texpageY;
    uint8_t u, v;            ///< top-left texel inside the texture page
    uint8_t bitDepth;        ///< 0 = 4 bpp, 1 = 8 bpp, 2 = 16 bpp
    uint8_t pad[3];
};
static_assert(sizeof(BackgroundTile) == 20, "BackgroundTile must be 20 bytes");

struct BackgroundLayer {
    uint32_t tilesOffset;    ///< tileCount BackgroundTile, from the pack start
    uint16_t tileCount;
    uint16_t nearSZ;         ///< GTE depth of the layer's nearest pixel
};
static_assert(sizeof(BackgroundLayer) == 8, "BackgroundLayer must be 8 bytes");

struct BackgroundRecord {
    uint32_t nameOffset;     ///< null-terminated name, or 0
    int32_t camX, camY, camZ;  ///< camera position, 20.12
    int16_t rot[9];          ///< world-to-view rotation, row major, 4.12
    int16_t angleX, angleY, angleZ;  ///< the same as Camera angles (Camera.GetRotation)
    uint16_t h;              ///< projection plane distance (GTE H)
    uint16_t tileCount;      ///< background tiles
    uint16_t layerCount;
    uint16_t hiddenCount;    ///< objects not drawn while this background shows
    uint16_t flags;          ///< BACKGROUND_FLAG_*
    uint16_t pad;
    uint32_t tilesOffset;
    uint32_t layersOffset;
    uint32_t hiddenOffset;   ///< hiddenCount uint16 GameObject indices
};
static_assert(sizeof(BackgroundRecord) == 64, "BackgroundRecord must be 64 bytes");

/// Shown when the scene loads (the first such record wins).
static constexpr uint16_t BACKGROUND_FLAG_SHOW_AT_START = 0x01;

/// The table at backgroundTableOffset: this header, then `count` records.
struct BackgroundTableHeader {
    uint16_t count;
    uint16_t reserved;
};
static_assert(sizeof(BackgroundTableHeader) == 4, "BackgroundTableHeader must be 4 bytes");

static constexpr int BACKGROUND_NONE = -1;

namespace backgroundmath {

/// Records in the table at `tableOffset` of `pack`; 0 for no table.
uint16_t count(const uint8_t* pack, uint32_t tableOffset);
/// Record `i`, or nullptr when out of range.
const BackgroundRecord* record(const uint8_t* pack, uint32_t tableOffset, int i);
/// Index of the record named `name`, or BACKGROUND_NONE.
int find(const uint8_t* pack, uint32_t tableOffset, const char* name);
/// The record flagged to show at start, or BACKGROUND_NONE.
int startIndex(const uint8_t* pack, uint32_t tableOffset);

/// Where the full image goes: in front of the dither TPage at the last bucket
/// and behind every world triangle that is not past the table.
int32_t imageBucket(uint32_t otSize);
/// A layer's bucket: its near depth, kept inside the world band and in front
/// of the image bucket.
int32_t layerBucket(uint16_t nearSZ, uint32_t otSize, int32_t worldDepthMin);

}  // namespace backgroundmath
}  // namespace psxsplash
