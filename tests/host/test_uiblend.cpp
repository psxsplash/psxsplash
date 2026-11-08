// Host tests for per-element UI blending (src/uiblend.hh), compiled natively.

#include "uiblend.hh"

#include "testing.hh"

using namespace psxsplash;
using namespace psxsplash::uiblend;

TEST(flags_without_blend_bits_are_opaque) {
    // Packs written before blending existed carry only the visible bit.
    CHECK_EQ((int)fromFlags(0x00), (int)UIBlend::Opaque);
    CHECK_EQ((int)fromFlags(0x01), (int)UIBlend::Opaque);
}

TEST(flags_decode_each_mode_next_to_visible) {
    CHECK_EQ((int)fromFlags(0x01 | (1 << 1)), (int)UIBlend::Half);
    CHECK_EQ((int)fromFlags(0x01 | (2 << 1)), (int)UIBlend::Add);
    CHECK_EQ((int)fromFlags(0x01 | (3 << 1)), (int)UIBlend::Subtract);
    CHECK_EQ((int)fromFlags(0x01 | (4 << 1)), (int)UIBlend::AddQuarter);
    // The visible bit does not leak into the blend, either way round.
    CHECK_EQ((int)fromFlags(4 << 1), (int)UIBlend::AddQuarter);
}

TEST(unused_codes_read_as_opaque) {
    CHECK_EQ((int)fromFlags(5 << 1), (int)UIBlend::Opaque);
    CHECK_EQ((int)fromFlags(6 << 1), (int)UIBlend::Opaque);
    CHECK_EQ((int)fromFlags(7 << 1), (int)UIBlend::Opaque);
    // Bits above the field are not part of it.
    CHECK_EQ((int)fromFlags(0x10 | (2 << 1)), (int)UIBlend::Add);
}

TEST(gpu_mode_numbering_matches_the_math) {
    // tpage bits 5-6: 0 = B/2+F/2, 1 = B+F, 2 = B-F, 3 = B+F/4.
    CHECK_EQ(gpuMode(UIBlend::Half), 0);
    CHECK_EQ(gpuMode(UIBlend::Add), 1);
    CHECK_EQ(gpuMode(UIBlend::Subtract), 2);
    CHECK_EQ(gpuMode(UIBlend::AddQuarter), 3);
    CHECK_EQ(blendChannel(UIBlend::Half, 20, 10), 15);
    CHECK_EQ(blendChannel(UIBlend::Add, 20, 20), 31);
    CHECK_EQ(blendChannel(UIBlend::Subtract, 10, 20), 0);
    CHECK_EQ(blendChannel(UIBlend::AddQuarter, 10, 20), 15);
    CHECK_EQ(blendChannel(UIBlend::Opaque, 10, 20), 20);
}

int main() { return psxsplash::test::runAll(); }
