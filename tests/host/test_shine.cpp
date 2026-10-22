// Host tests for shine (environment-mapped reflections): the shine table walk
// and the reflection UV and overlay arithmetic of src/shinemath.hh, compiled
// natively. The renderer feeds envCoord the GTE's RT * normal (4.12, view y
// down the screen).

#include <vector>

#include "shinemath.hh"

#include "testing.hh"

using namespace psxsplash;
using namespace psxsplash::shinemath;

namespace {
void put16(std::vector<uint8_t>& t, uint16_t v) { t.push_back(v & 0xFF); t.push_back(v >> 8); }
// One record and its tris x 3 normals, normal k of tri i = (i, k, obj).
void putRecord(std::vector<uint8_t>& t, uint16_t obj, uint16_t tris, uint8_t strength, uint8_t blend) {
    put16(t, obj); put16(t, tris); put16(t, 0x0307); put16(t, 3); put16(t, 481);
    t.push_back(16); t.push_back(32); t.push_back(63); t.push_back(127); t.push_back(strength); t.push_back(blend);
    for (uint16_t i = 0; i < tris; i++)
        for (int k = 0; k < 3; k++) { put16(t, i); put16(t, uint16_t(k)); put16(t, obj); }
}
}  // namespace

TEST(record_layout) {
    CHECK_EQ(sizeof(ShineRecord), 16);
    std::vector<uint8_t> t = {1, 0, 0, 0};
    putRecord(t, 2, 1, 64, SHINE_HALF);
    const ShineRecord* r = reinterpret_cast<const ShineRecord*>(t.data() + 4);
    CHECK_EQ(r->gameObjectIndex, 2);
    CHECK_EQ(r->triCount, 1);
    CHECK_EQ(r->tpage, 0x0307);
    CHECK_EQ(r->clutX, 3);
    CHECK_EQ(r->clutY, 481);
    CHECK_EQ(r->u0, 16);
    CHECK_EQ(r->v0, 32);
    CHECK_EQ(r->wMinus1, 63);
    CHECK_EQ(r->hMinus1, 127);
    CHECK_EQ(r->strength, 64);
    CHECK_EQ(r->blend, SHINE_HALF);
    CHECK_EQ(normals(r, 0)[0], 0);
    CHECK_EQ(normals(r, 0)[4], 1);  // second corner's y: its corner index
    CHECK_EQ(normals(r, 0)[5], 2);  // and z: the object
    CHECK_EQ(normals(r, 0)[8], 2);
}

TEST(table_walk_by_object) {
    // Records of 3, 1 and 2 tris back to back (the second at a 2-byte, not
    // 4-byte, boundary), one past the objects, one whose tri count disagrees
    // with its object's, one with an unknown blend.
    std::vector<uint8_t> t = {6, 0, 0, 0};
    putRecord(t, 4, 3, 128, SHINE_ADD);
    putRecord(t, 1, 1, 64, SHINE_HALF);
    putRecord(t, 9, 2, 64, SHINE_ADD);   // object 9 of 6
    putRecord(t, 2, 5, 64, SHINE_ADD);   // object 2 has 2 tris
    putRecord(t, 3, 2, 64, 7);           // blend 7
    putRecord(t, 5, 2, 32, SHINE_ADD);
    const uint16_t polys[6] = {0, 1, 2, 2, 3, 2};
    const ShineRecord* by[6] = {};
    CHECK_EQ(parseTable(t.data(), 6, [&](uint16_t i) { return polys[i]; }, by), 3);
    CHECK(by[0] == nullptr);
    CHECK(by[2] == nullptr);
    CHECK(by[3] == nullptr);
    CHECK(by[4] != nullptr && by[4]->triCount == 3 && by[4]->strength == 128);
    CHECK(by[1] != nullptr && by[1]->blend == SHINE_HALF);
    CHECK(by[5] != nullptr && by[5]->strength == 32);
    // Each record finds its own normals: tri i, corner k is (i, k, object).
    CHECK_EQ(normals(by[4], 2)[6], 2);
    CHECK_EQ(normals(by[4], 2)[7], 2);
    CHECK_EQ(normals(by[4], 2)[8], 4);
    CHECK_EQ(normals(by[5], 1)[3], 1);
    CHECK_EQ(normals(by[5], 1)[8], 5);
    // Empty table.
    uint8_t empty[4] = {0, 0, 0, 0};
    const ShineRecord* none[6] = {};
    CHECK_EQ(parseTable(empty, 6, [&](uint16_t i) { return polys[i]; }, none), 0);
}

TEST(env_coord_maps_the_normal) {
    // 64 wide at u0 = 16: facing the camera is the middle, +x the right edge,
    // -x the left.
    CHECK_EQ(envCoord(0, 16, 63), 16 + 32);
    CHECK_EQ(envCoord(4096, 16, 63), 16 + 63);
    CHECK_EQ(envCoord(-4096, 16, 63), 16);
    CHECK_EQ(envCoord(2048, 16, 63), 16 + 48);
    CHECK_EQ(envCoord(-2048, 16, 63), 16 + 16);
    // View y points down: a normal facing up (ny < 0) reads the top rows.
    CHECK(envCoord(-3000, 0, 63) < envCoord(0, 0, 63));
    CHECK(envCoord(3000, 0, 63) > envCoord(0, 0, 63));
    // Monotonic across the whole range.
    for (int32_t n = -4096; n < 4096; n += 64) CHECK(envCoord(n, 0, 127) <= envCoord(n + 64, 0, 127));
}

TEST(env_coord_stays_in_its_rectangle) {
    // A scaled object or a saturated IR can pass 4096: clamped, never into the
    // neighbouring texture.
    CHECK_EQ(envCoord(32767, 100, 31), 131);
    CHECK_EQ(envCoord(-32768, 100, 31), 100);
    // A full 256 texel page: no wrap past 255.
    CHECK_EQ(envCoord(4096, 0, 255), 255);
    CHECK_EQ(envCoord(-4096, 0, 255), 0);
    CHECK_EQ(envCoord(0, 0, 255), 128);
    for (int32_t n = -8192; n <= 8192; n += 512) {
        uint8_t c = envCoord(n, 192, 63);
        CHECK(c >= 192 && c <= 255);
    }
}

TEST(overlay_fades_with_the_fog) {
    CHECK_EQ(overlayGrey(128, 0), 128);
    CHECK_EQ(overlayGrey(128, -5), 128);
    CHECK_EQ(overlayGrey(128, 4096), 0);
    CHECK_EQ(overlayGrey(128, 5000), 0);
    CHECK_EQ(overlayGrey(128, 2048), 64);
    CHECK_EQ(overlayGrey(64, 1024), 48);
    // As the base texture fades (renderer fogTexColor keeps c * (4096 - ir) >> 12).
    for (int32_t ir = 0; ir <= 4096; ir += 256) CHECK_EQ(overlayGrey(100, ir), (100 * (4096 - ir)) >> 12);
}

int main() { return psxsplash::test::runAll(); }
