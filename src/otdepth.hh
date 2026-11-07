#pragma once

#include <stdint.h>

// Ordering-table depth biases, free of psyqo so that tests/host/test_decal.cpp
// compiles this exact code natively.
//
// A world triangle goes in the bucket of its deepest corner, max(SZ), floored
// at the renderer's WORLD_DEPTH_MIN. Within a bucket the first fragment
// inserted is drawn last (on top), so two coplanar triangles that share their
// deepest corner tie, and which one shows depends on traversal order; a
// coplanar triangle under a bigger one that the renderer split also loses to
// the nearer children.

namespace psxsplash {
namespace otdepth {

// Blob shadows (shadowrender.cpp): the bucket is the blob centre's depth less
// this, in front of the floor under it.
static constexpr int32_t kShadowDepthBias = 8;

// Decal triangles (Tri flag DECAL_FLAG, mesh.hh): the exporter clipped them to
// the surface under them and they lie on it, so they go this many buckets in
// front of their own max(SZ): a piece holding its surface triangle's deepest
// corner ties with it otherwise, and lost about half its pixels in Redux at
// every view measured. 1, 4 and 20 won them all alike: 2 is the smallest
// that worked plus one SZ for the exporter's rounding of the cut corners.
// One SZ is gteScaling / 4096 m: 2 is 0.5 cm at gteScaling 10, 4.9 cm at
// gteScaling 100.
// The other side of the bias: a triangle in front of the surface whose deepest
// corner is less than 2 SZ nearer than the decal piece's (a thin poster on the
// wall, a foot or a crate against it) is drawn before the decal, so where they
// overlap on screen the decal shows through it (at exactly 2, insertion order
// decides). Bigger K widens that band: 4 was 10 cm at gteScaling 100.
static constexpr int32_t kDecalDepthBias = 2;

// The bucket of a decal triangle whose plain bucket (max(SZ), already floored)
// is zIndex.
static inline int32_t decalBucket(int32_t zIndex, int32_t worldDepthMin) {
    const int32_t z = zIndex - kDecalDepthBias;
    return z < worldDepthMin ? worldDepthMin : z;
}

}  // namespace otdepth
}  // namespace psxsplash
