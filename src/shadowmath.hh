#pragma once

#include <stdint.h>

// Blob shadow arithmetic, free of psyqo and the GTE so that
// tests/host/test_shadow.cpp compiles this exact code natively. The renderer
// (renderer.cpp renderShadows) finds the floor and projects; what a blob looks
// like for a given floor distance is decided here.
//
// The floor is the nav region under the object; with no region there, the
// bottom of its bounds, unless the scene has nav and the object has moved
// (then it is in the air off the floor and gets no blob).
//
// A blob is a fan of eight triangles on the floor under its object: the centre
// vertex at the shadow's darkness, the rim at 0, drawn with the GPU's subtract
// blend so it darkens whatever floor is already there. With h the height of
// the object's bottom above that floor:
//
//   fade     = max(0, 1 - h / fadeHeight)
//   radius   = size * fade
//   darkness = darkness * fade * edge * fog
//
// edge fades the blob out as its centre nears the end of the floor (within
// radius of a nav region's walk-off edge: other region edges are a player
// radius in from a wall or a drop, so the floor goes on past them), fog as
// the centre goes into the fog. SplashEdit's scene view draws the same formula
// (core/shadow.hh in splashedit-ng).

namespace psxsplash {

/// Shadows a scene can hold; the loader drops the rest.
static constexpr int MAX_SCENE_SHADOWS = 32;

enum ShadowMode : uint8_t {
    SHADOW_OFF = 0,
    SHADOW_BLOB = 1,
    SHADOW_SILHOUETTE = 2,  ///< reserved; drawn as nothing, like any unknown mode
};

/// A shadow as stored in the splashpack (v28). The shadow table is a uint16
/// count and a uint16 pad, then `count` of these.
struct ShadowRecord {
    uint16_t gameObjectIndex;
    uint8_t mode;         ///< ShadowMode
    uint8_t darkness;     ///< 0..255, subtracted from the floor at the centre
    uint16_t size;        ///< blob radius on the ground, 20.12 position units
    uint16_t fadeHeight;  ///< height at which the blob is gone, 20.12 position units
};
static_assert(sizeof(ShadowRecord) == 8, "ShadowRecord must be 8 bytes");

namespace shadowmath {

static constexpr int kRimVerts = 8;
/// cos / sin of i * 45 degrees, 4.12.
static constexpr int32_t kRimCos[kRimVerts] = {4096, 2896, 0, -2896, -4096, -2896, 0, 2896};
static constexpr int32_t kRimSin[kRimVerts] = {0, 2896, 4096, 2896, 0, -2896, -4096, -2896};

/// Copies the blob records of a shadow table (`table` points at its count)
/// whose object index is below objectCount into out, at most max of them, and
/// returns how many. Records of any other mode are left out.
inline int parseTable(const uint8_t* table, uint16_t objectCount, ShadowRecord* out, int max) {
    uint16_t count = (uint16_t)(table[0] | (table[1] << 8));
    const uint8_t* p = table + 4;
    int n = 0;
    for (uint16_t i = 0; i < count && n < max; i++, p += sizeof(ShadowRecord)) {
        ShadowRecord r;
        r.gameObjectIndex = (uint16_t)(p[0] | (p[1] << 8));
        r.mode = p[2];
        r.darkness = p[3];
        r.size = (uint16_t)(p[4] | (p[5] << 8));
        r.fadeHeight = (uint16_t)(p[6] | (p[7] << 8));
        if (r.mode != SHADOW_BLOB || r.gameObjectIndex >= objectCount) continue;
        out[n++] = r;
    }
    return n;
}

/// How much of the blob is left at height h above the floor, 4.12: 4096 on
/// the floor (or below it), 0 at fadeHeight and above.
inline int32_t heightFade(int32_t h, int32_t fadeHeight) {
    if (h <= 0) return 4096;
    if (fadeHeight <= 0 || h >= fadeHeight) return 0;
    // fadeHeight is a uint16, so the shifted numerator fits 32 bits (the
    // engine links no 64-bit division).
    return ((fadeHeight - h) << 12) / fadeHeight;
}

/// Fade for a centre edgeDist from the end of the floor, 4.12: 4096 at radius
/// or further, falling linearly to 0 at the edge.
inline int32_t edgeFade(int32_t edgeDist, int32_t radius) {
    if (radius <= 0 || edgeDist >= radius) return 4096;
    if (edgeDist <= 0) return 0;
    return (edgeDist << 12) / radius;  // edgeDist < radius <= 0xFFFF
}

/// The renderer's per-vertex fog amount at depth sz (processTriangle), 4.12.
inline int32_t fogAmount(int32_t sz, int32_t fogFarSZ) {
    if (fogFarSZ <= 0) return 0;
    int32_t fogNear = fogFarSZ >> 3;
    int32_t range = fogFarSZ - fogNear;
    if (range < 1) range = 1;
    if (sz <= fogNear) return 0;
    if (sz >= fogFarSZ) return 4096;
    return ((sz - fogNear) * 4096) / range;
}

inline int32_t blobRadius(uint16_t size, int32_t fade) { return (int32_t)(((int64_t)size * fade) >> 12); }

/// The centre's colour: darkness scaled by three 4.12 factors.
inline uint8_t blobDarkness(uint8_t darkness, int32_t fade, int32_t edge, int32_t fogKeep) {
    int64_t d = (int64_t)darkness * fade;
    d = (d * edge) >> 12;
    d = (d * fogKeep) >> 24;
    if (d < 0) d = 0;
    if (d > 255) d = 255;
    return (uint8_t)d;
}

}  // namespace shadowmath
}  // namespace psxsplash
