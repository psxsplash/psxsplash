// Host tests for Lua's UI.DrawTexturedTriangle arithmetic (src/uitextri.hh),
// compiled natively.

#include "uitextri.hh"

#include "testing.hh"

using namespace psxsplash;
using namespace psxsplash::uitextri;

TEST(uv_adds_the_sheet_base) {
    // A sheet packed at u0 = 64, v0 = 128 in its page: texel (0,0) of the
    // texture is page texel (64,128).
    CHECK_EQ(texCoord(64, 0), 64);
    CHECK_EQ(texCoord(128, 0), 128);
    CHECK_EQ(texCoord(64, 31), 95);
    CHECK_EQ(texCoord(0, 17), 17);
}

TEST(uv_inside_a_sheet_that_fits_its_page_is_exact) {
    // The exporter guarantees base + size <= 256, so the last texel of a
    // texture that ends at the page edge is 255, not wrapped.
    CHECK_EQ(texCoord(192, 63), 255);
    CHECK_EQ(texCoord(0, 255), 255);
    for (int base = 0; base < 256; base += 16)
        for (int rel = 0; base + rel < 256; rel += 7) CHECK_EQ(texCoord((uint8_t)base, rel), base + rel);
}

TEST(uv_past_the_page_clamps_rather_than_wraps) {
    // 192 + 64 = 256 would wrap to 0, the far side of the page.
    CHECK_EQ(texCoord(192, 64), 255);
    CHECK_EQ(texCoord(200, 1000), 255);
    CHECK_EQ(texCoord(10, -11), 0);
    CHECK_EQ(texCoord(0, -1), 0);
    CHECK_EQ(texCoord(10, -10), 0);
}

TEST(blend_names) {
    CHECK_EQ((int)blendFromName("half"), (int)UIBlend::Half);
    CHECK_EQ((int)blendFromName("add"), (int)UIBlend::Add);
    CHECK_EQ((int)blendFromName("subtract"), (int)UIBlend::Subtract);
    CHECK_EQ((int)blendFromName("addQuarter"), (int)UIBlend::AddQuarter);
    CHECK_EQ((int)blendFromName("opaque"), (int)UIBlend::Opaque);
    // Exact match only: no prefix, no case folding, no trailing text.
    CHECK_EQ((int)blendFromName("ad"), (int)UIBlend::Opaque);
    CHECK_EQ((int)blendFromName("addq"), (int)UIBlend::Opaque);
    CHECK_EQ((int)blendFromName("Half"), (int)UIBlend::Opaque);
    CHECK_EQ((int)blendFromName("halfx"), (int)UIBlend::Opaque);
    CHECK_EQ((int)blendFromName(""), (int)UIBlend::Opaque);
    CHECK_EQ((int)blendFromName(nullptr), (int)UIBlend::Opaque);
}

TEST(blend_codes_are_the_uiblend_codes) {
    CHECK_EQ((int)blendFromCode(0), (int)UIBlend::Opaque);
    CHECK_EQ((int)blendFromCode(1), (int)UIBlend::Half);
    CHECK_EQ((int)blendFromCode(2), (int)UIBlend::Add);
    CHECK_EQ((int)blendFromCode(3), (int)UIBlend::Subtract);
    CHECK_EQ((int)blendFromCode(4), (int)UIBlend::AddQuarter);
    CHECK_EQ((int)blendFromCode(5), (int)UIBlend::Opaque);
    CHECK_EQ((int)blendFromCode(-1), (int)UIBlend::Opaque);
    // And they reach the tpage bits the same way UI elements do.
    CHECK_EQ(uiblend::gpuMode(blendFromName("addQuarter")), 3);
    CHECK_EQ(uiblend::gpuMode(blendFromCode(1)), 0);
}

TEST(a_missing_name_is_reported_once) {
    ReportedNames r;
    CHECK(r.first(nameKey("ghost")));
    CHECK(!r.first(nameKey("ghost")));
    CHECK(!r.first(nameKey("ghost")));
    CHECK(r.first(nameKey("ghosts")));
    CHECK(r.first(indexKey(7)));
    CHECK(!r.first(indexKey(7)));
    CHECK(r.first(indexKey(-1)));
    CHECK_EQ(r.count, 4);
}

TEST(the_table_is_fixed_and_stops_logging_when_full) {
    ReportedNames r;
    for (int i = 0; i < ReportedNames::kMax; i++) CHECK(r.first(indexKey(i)));
    CHECK(!r.first(indexKey(ReportedNames::kMax)));
    CHECK(!r.first(indexKey(0)));
    CHECK_EQ(r.count, ReportedNames::kMax);
    r.clear();
    CHECK(r.first(indexKey(0)));
}

TEST(name_keys_differ) {
    CHECK(nameKey("hero") != nameKey("fx"));
    CHECK(nameKey("") == nameKey(nullptr));
    CHECK(nameKey("ab") != nameKey("ba"));
}

int main() { return psxsplash::test::runAll(); }
