#pragma once

#include <stdint.h>

// Dynamic point-light arithmetic, free of psyqo and the GTE so that
// tests/host/test_light.cpp can compile this exact code natively. The renderer
// (renderer.cpp) feeds it dot products it computes on the GTE; everything else
// about a light - culling, the object-space transform, falloff and N.L - is here.
//
// Model, per vertex:
//
//   colour = clamp(baked + sum_i colour_i * intensity_i * (1 - d/r) * max(0, N.L))
//
// d is the vertex-to-light distance, r the light radius. N is the triangle's
// stored normal, which the splashpack carries ONCE per triangle (the normal of
// its first vertex), so all three vertices share it.

namespace psxsplash {

/// Lights the scene can hold. Lua can enable, disable and move any of them.
static constexpr int MAX_SCENE_LIGHTS = 16;

/// Lights that can affect one mesh at once. A mesh touched by more keeps the
/// first four in scene order. Four is what the GTE can dot against a vertex in
/// two MVMVAs: three rows of the light matrix and one row of the colour matrix.
static constexpr int MAX_LIGHTS_PER_MESH = 4;

/// A point light in world space. Positions and radius share the fixed-point
/// 20.12 space of GameObject positions.
struct PointLight {
    int32_t x, y, z;
    int32_t radius;
    uint16_t intensity;  ///< 4.12, 4096 = 1.0
    uint8_t r, g, b;
    uint8_t enabled;
};

namespace lightmath {

/// Positions are shifted right until every coordinate and radius is at most
/// this, which keeps every operand inside a GTE matrix entry and every sum in
/// term() inside 32 bits (12 * 8192^2 < 2^31). The shift is never less than 2,
/// so the int16 vertex coordinates fit too.
static constexpr int32_t kOperandMax = 8191;
static constexpr int kMinShift = 2;

/// Rounding arithmetic shift, used for every position that goes into the dot
/// products so the light and the vertex are quantised the same way.
inline int32_t shiftRound(int32_t v, int shift) { return (v + (1 << (shift - 1))) >> shift; }

/// One light prepared for one object: moved into the object's local space and
/// scaled by that object's shift.
struct ObjectLight {
    int32_t lx, ly, lz;     ///< object-local position, >> shift
    int32_t lenSq;          ///< lx*lx + ly*ly + lz*lz
    int32_t radius;         ///< radius >> shift, at least 1
    int32_t radiusSq;       ///< radius * radius
    uint32_t invRadiusSq;   ///< 2^(invShift + 16) / radiusSq, kept in 31 bits
    uint8_t invShift;
    int32_t invRadius;      ///< (1 << 24) / radius
    int32_t cr, cg, cb;     ///< colour * intensity, 0..4080
};

struct ObjectLights {
    ObjectLight lights[MAX_LIGHTS_PER_MESH];
    int count = 0;
    int shift = kMinShift;
};

/// True if the sphere (cx,cy,cz,radius) reaches the box. All 20.12.
bool sphereTouchesAABB(int32_t cx, int32_t cy, int32_t cz, int32_t radius,
                       const int32_t boxMin[3], const int32_t boxMax[3]);

/// Select the enabled lights reaching the object's world AABB and move them into
/// its local space. `rot` is the object's rotation, rows first, 4.12; local =
/// rot^T * (light - position). Returns the number of lights kept.
int prepare(const PointLight* lights, int lightCount, const int32_t position[3],
            const int32_t rot[3][3], const int32_t aabbMin[3], const int32_t aabbMax[3],
            ObjectLights& out);

/// k(t) = (1 - sqrt(t)) / sqrt(t) at t = i/256, 6.10. Entries below
/// kExactBins are unused: term() takes termExact() there.
struct FalloffLut {
    uint16_t k[257];
};
extern const FalloffLut kFalloffLut;
static constexpr uint32_t kExactBins = 16;

/// term() within r/4 of the light, where 1/sqrt(t) is too steep for the table:
/// a square root and two divides.
int32_t termExact(const ObjectLight& light, int32_t distSq, int32_t nd);

/// (a * b) >> s for s in [16, 48], without a variable 64-bit shift helper.
inline uint32_t mulShr(uint32_t a, uint32_t b, int s) {
    uint64_t p = (uint64_t)a * b;
    uint32_t hi = (uint32_t)(p >> 32), lo = (uint32_t)p;
    if (s >= 32) return hi >> (s - 32);
    return (hi << (32 - s)) | (lo >> s);
}

/// Falloff times cosine for one vertex and one light, 4.12 (0..4096), or 0 if
/// the vertex is out of range or faces away. The dot products are the ones the
/// renderer gets from the GTE, all on shifted operands:
///   lDotV = L.v, vLenSq = v.v, nDotL = N.L, nDotV = N.v
/// with N the 4.12 normal and L, v the shifted light and vertex positions.
///
/// The table multiplies k by (N.d) / r, which is cos * sqrt(t), so the product
/// is cos * (1 - d/r) with no square root and no divide.
inline int32_t term(const ObjectLight& light, int32_t lDotV, int32_t vLenSq, int32_t nDotL,
                    int32_t nDotV) {
    int32_t distSq = light.lenSq - 2 * lDotV + vLenSq;
    if (distSq >= light.radiusSq) return 0;
    int32_t nd = nDotL - nDotV;
    if (nd < 0) return 0;
    if (distSq <= 0) return 4096;

    // t = distSq / radiusSq as 0.16.
    uint32_t t16 = mulShr((uint32_t)distSq, light.invRadiusSq, light.invShift);
    uint32_t bin = t16 >> 8;
    if (bin >= 256) return 0;
    if (bin < kExactBins) return termExact(light, distSq, nd);

    uint32_t f = t16 & 0xFF;
    const uint16_t* lut = kFalloffLut.k;
    int32_t k = (int32_t)((lut[bin] * (256 - f) + lut[bin + 1] * f) >> 8);
    int32_t q = (int32_t)(((int64_t)nd * light.invRadius) >> 24);  // cos * d / r, 4.12
    int32_t t = (q * k) >> 10;
    return t > 4096 ? 4096 : t;
}

/// Bit i set if light i can reach the triangle, judged per axis against the
/// triangle's box. Conservative: a clear bit is certain, a set bit is not. `v`
/// holds the three shifted vertex positions.
uint32_t reachMask(const ObjectLights& ol, const int32_t v[3][3]);

/// baked + add, clamped to a colour byte.
inline uint8_t addClamp(uint8_t baked, int32_t add) {
    int32_t v = baked + add;
    return v > 255 ? 255 : (uint8_t)v;
}

}  // namespace lightmath
}  // namespace psxsplash
