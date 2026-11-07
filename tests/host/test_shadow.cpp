// Host tests for blob shadows: the v28/v29 header size mapping
// (src/splashpackversion.hh) and the shadow table and fade math
// (src/shadowmath.hh), compiled natively.

#include <math.h>

#include "shadowmath.hh"
#include "splashpackversion.hh"

#include "testing.hh"

using namespace psxsplash;
using namespace psxsplash::shadowmath;

TEST(header_size_per_version) {
    CHECK_EQ(splashpackHeaderSize(20), 120);
    CHECK_EQ(splashpackHeaderSize(21), 128);
    CHECK_EQ(splashpackHeaderSize(22), 144);
    CHECK_EQ(splashpackHeaderSize(23), 144);
    CHECK_EQ(splashpackHeaderSize(24), 148);
    CHECK_EQ(splashpackHeaderSize(25), 156);
    CHECK_EQ(splashpackHeaderSize(26), 156);
    CHECK_EQ(splashpackHeaderSize(27), 156);
    CHECK_EQ(splashpackHeaderSize(28), 160);
    CHECK_EQ(splashpackHeaderSize(29), 164);
    CHECK_EQ(splashpackHeaderSize(30), 164);
    // v28 appends exactly one u32, the shadow table offset; v29 one more, the
    // background table offset.
    CHECK_EQ(kSplashpackHeaderSizeV28 - kSplashpackHeaderSizeV25, 4);
    CHECK_EQ(kSplashpackHeaderSizeV29 - kSplashpackHeaderSizeV28, 4);
}

namespace {
void putRecord(uint8_t* p, uint16_t obj, uint8_t mode, uint8_t dark, uint16_t size, uint16_t fade) {
    p[0] = obj & 0xFF; p[1] = obj >> 8;
    p[2] = mode; p[3] = dark;
    p[4] = size & 0xFF; p[5] = size >> 8;
    p[6] = fade & 0xFF; p[7] = fade >> 8;
}
}  // namespace

TEST(table_parse_keeps_blobs_only) {
    uint8_t t[4 + 4 * 8] = {4, 0, 0, 0};
    putRecord(t + 4, 3, SHADOW_BLOB, 153, 20, 102);
    putRecord(t + 12, 4, SHADOW_SILHOUETTE, 200, 20, 102);  // reserved: off
    putRecord(t + 20, 900, SHADOW_BLOB, 10, 20, 102);       // past the objects
    putRecord(t + 28, 0x0102, SHADOW_BLOB, 255, 0x1234, 0xBEEF);
    ShadowRecord out[4];
    CHECK_EQ(parseTable(t, 500, out, 4), 2);
    CHECK_EQ(out[0].gameObjectIndex, 3);
    CHECK_EQ(out[0].mode, SHADOW_BLOB);
    CHECK_EQ(out[0].darkness, 153);
    CHECK_EQ(out[0].size, 20);
    CHECK_EQ(out[0].fadeHeight, 102);
    CHECK_EQ(out[1].gameObjectIndex, 0x0102);
    CHECK_EQ(out[1].size, 0x1234);
    CHECK_EQ(out[1].fadeHeight, 0xBEEF);
    // The cap.
    CHECK_EQ(parseTable(t, 500, out, 1), 1);
    // Empty table.
    uint8_t empty[4] = {0, 0, 0, 0};
    CHECK_EQ(parseTable(empty, 500, out, 4), 0);
}

TEST(height_fade) {
    CHECK_EQ(heightFade(0, 100), 4096);
    CHECK_EQ(heightFade(-5, 100), 4096);  // sunk into the floor
    CHECK_EQ(heightFade(50, 100), 2048);
    CHECK_EQ(heightFade(25, 100), 3072);
    CHECK_EQ(heightFade(100, 100), 0);
    CHECK_EQ(heightFade(150, 100), 0);
    CHECK_EQ(heightFade(1, 0), 0);
}

TEST(edge_fade) {
    CHECK_EQ(edgeFade(0x7FFFFFFF, 20), 4096);  // no edge
    CHECK_EQ(edgeFade(20, 20), 4096);
    CHECK_EQ(edgeFade(10, 20), 2048);
    CHECK_EQ(edgeFade(0, 20), 0);
    CHECK_EQ(edgeFade(5, 0), 4096);
}

TEST(darkness_and_radius) {
    CHECK_EQ(blobDarkness(200, 4096, 4096, 4096), 200);
    CHECK_EQ(blobDarkness(200, 2048, 4096, 4096), 100);
    CHECK_EQ(blobDarkness(200, 2048, 2048, 4096), 50);
    CHECK_EQ(blobDarkness(200, 4096, 4096, 0), 0);
    CHECK_EQ(blobDarkness(255, 4096, 4096, 4096), 255);
    CHECK_EQ(blobRadius(20, 4096), 20);
    CHECK_EQ(blobRadius(20, 2048), 10);
    CHECK_EQ(blobRadius(65535, 4096), 65535);
}

TEST(fog_amount_matches_renderer) {
    // processTriangle: 0 up to far/8, 4096 from far, linear between.
    CHECK_EQ(fogAmount(100, 0), 0);
    CHECK_EQ(fogAmount(100, 800), 0);
    CHECK_EQ(fogAmount(800, 800), 4096);
    CHECK_EQ(fogAmount(450, 800), ((450 - 100) * 4096) / 700);
}

TEST(rim_is_a_circle) {
    for (int i = 0; i < kRimVerts; i++) {
        double a = i * M_PI / 4;
        CHECK(fabs(kRimCos[i] - 4096 * cos(a)) < 1);
        CHECK(fabs(kRimSin[i] - 4096 * sin(a)) < 1);
    }
}

int main() { return psxsplash::test::runAll(); }
