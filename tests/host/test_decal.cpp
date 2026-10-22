// Host tests for decal ordering (src/otdepth.hh), compiled natively: a decal
// triangle (Tri flag DECAL_FLAG) goes kDecalDepthBias buckets in front of its
// own deepest corner, floored like every world triangle.

#include "otdepth.hh"

#include "testing.hh"

using namespace psxsplash::otdepth;

namespace {
constexpr int32_t kWorldMin = 14;  // Renderer::WORLD_DEPTH_MIN (SPRITE_DEPTH_BASE + SPRITE_LAYERS)
// The plain bucket of a world triangle: its deepest corner, floored (renderer.cpp processTriangle).
int32_t plainBucket(int32_t sz0, int32_t sz1, int32_t sz2) {
    int32_t z = sz0 > sz1 ? sz0 : sz1;
    z = z > sz2 ? z : sz2;
    return z < kWorldMin ? kWorldMin : z;
}
}  // namespace

TEST(decal_sharing_the_deepest_corner_goes_in_front) {
    // The exporter's pieces are clipped inside the surface triangle, so a piece
    // that holds the surface's deepest corner has the same max(SZ): a tie, which
    // insertion order would decide. The bias breaks it towards the decal.
    const int32_t surface = plainBucket(1200, 900, 1000);
    const int32_t decal = decalBucket(plainBucket(1200, 1100, 1150), kWorldMin);
    CHECK(decal < surface);
    CHECK_EQ(surface - decal, kDecalDepthBias);
}

TEST(decal_beats_a_split_child_within_the_bias) {
    // A split child of the surface under the decal ends kDecalDepthBias - 1 SZ
    // nearer than the decal piece over it: the decal is still drawn after it.
    const int32_t piece = plainBucket(2000, 1990, 1980);
    const int32_t child = plainBucket(2000 - (kDecalDepthBias - 1), 1985, 1970);
    CHECK(decalBucket(piece, kWorldMin) < child);
}

TEST(decal_bucket_floors_at_world_min) {
    CHECK_EQ(decalBucket(kWorldMin, kWorldMin), kWorldMin);
    CHECK_EQ(decalBucket(kWorldMin + kDecalDepthBias - 1, kWorldMin), kWorldMin);
    CHECK_EQ(decalBucket(kWorldMin + kDecalDepthBias, kWorldMin), kWorldMin);
    CHECK_EQ(decalBucket(kWorldMin + kDecalDepthBias + 1, kWorldMin), kWorldMin + 1);
    // Never into the reserved sprite / UI bands, whatever the depth.
    for (int32_t z = kWorldMin; z < kWorldMin + 4 * kDecalDepthBias; z++) CHECK(decalBucket(z, kWorldMin) >= kWorldMin);
}

TEST(decal_bias_is_small_in_metres) {
    // 1.0 in the engine's 4.12 fixed point is gteScaling metres: the bias must
    // stay under 6 cm even at gteScaling 100, or a decal on a wall would come
    // through a foot or a crate standing against it.
    CHECK(kDecalDepthBias > 0);
    CHECK(kDecalDepthBias * 100.0 / 4096.0 < 0.06);
    CHECK(kDecalDepthBias * 10.0 / 4096.0 < 0.006);
}

TEST(shadow_bias_unchanged) {
    CHECK_EQ(kShadowDepthBias, 8);
}

int main() { return ::psxsplash::test::runAll(); }
