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
    uint8_t tShift;         ///< radiusSq << tShift lands in [2^31, 2^32)
    uint32_t invRadiusSq;   ///< 2^52 / (radiusSq << tShift)
    uint32_t invRadius;     ///< 0xFFFFFFFF / radius
    int32_t cr, cg, cb;     ///< colour * intensity, 0..4080
    uint8_t sceneIndex;     ///< which PointLight this came from
    /// flatColour() for every t >> kColourLutShift, or null to compute it.
    const uint32_t* colourLut;
};

struct ObjectLights {
    ObjectLight lights[MAX_LIGHTS_PER_MESH];
    int count = 0;
    int shift = kMinShift;
    /// -(4096 >> shift): the diagonal of the GTE matrix that takes a raw vertex
    /// to shifted units in the flat path.
    int32_t flatScale = 0;
};

/// light - vertex on one axis, in shifted units, rounded the way the GTE's
/// MVMVA does it (sf=1): ((L << 12) + flatScale * v) >> 12.
inline int32_t flatDelta(const ObjectLights& ol, int32_t l, int32_t v) {
    return ((l << 12) + ol.flatScale * v) >> 12;
}

/// True if the sphere (cx,cy,cz,radius) reaches the box. All 20.12.
bool sphereTouchesAABB(int32_t cx, int32_t cy, int32_t cz, int32_t radius,
                       const int32_t boxMin[3], const int32_t boxMax[3]);

/// Select the enabled lights reaching the object's world AABB and move them into
/// its local space. `rot` is the object's rotation, rows first, 4.12; local =
/// rot^T * (light - position). Returns the number of lights kept.
int prepare(const PointLight* lights, int lightCount, const int32_t position[3],
            const int32_t rot[3][3], const int32_t aabbMin[3], const int32_t aabbMax[3],
            ObjectLights& out);

/// k(t) = (1 - sqrt(t)) / sqrt(t) at the centre of bin i of t = d^2/r^2 in
/// 1024ths, 6.10. Entries below kExactBins are unused: shade() takes
/// termExact() there.
static constexpr uint32_t kLutBins = 1024;
struct FalloffLut {
    uint16_t k[kLutBins];
};
extern const FalloffLut kFalloffLut;
static constexpr uint32_t kExactBins = kLutBins / 16;

/// High word of a 32x32 unsigned product: one multu and an mfhi.
inline uint32_t mulhi(uint32_t a, uint32_t b) { return (uint32_t)(((uint64_t)a * b) >> 32); }

/// term() within r/4 of the light, where 1/sqrt(t) is too steep for the table:
/// a square root and two divides.
int32_t termExact(const ObjectLight& light, int32_t distSq, int32_t nd);

/// Falloff times cosine for one vertex and one light, 4.12 (0..4096), or 0 if
/// the vertex is out of range or faces away. The dot products are the ones the
/// renderer gets from the GTE, all on shifted operands:
///   lDotV = L.v, vLenSq = v.v, nDotL = N.L, nDotV = N.v
/// with N the 4.12 normal and L, v the shifted light and vertex positions.
///
/// The table multiplies k by (N.d) / r, which is cos * sqrt(t), so the product
/// is cos * (1 - d/r) with no square root and no divide.
/// The same, from the squared distance and N.d (d = light - point, shifted).
/// The flat path's colour quantises t to 129 steps, so a light's whole colour
/// response is a 129-entry table the renderer keeps per light.
static constexpr int kColourLutShift = 5;
static constexpr int kColourLutSize = (4096 >> kColourLutShift) + 1;

/// Packed 0x00BBGGRR of colour * t, each channel saturated to 255, with t
/// quantised to the colour table's step.
inline uint32_t flatColour(int32_t cr, int32_t cg, int32_t cb, int32_t t) {
    const int32_t tq = (t >> kColourLutShift) << kColourLutShift;
    int32_t r = (cr * tq) >> 12, g = (cg * tq) >> 12, b = (cb * tq) >> 12;
    if (r > 255) r = 255;
    if (g > 255) g = 255;
    if (b > 255) b = 255;
    return (uint32_t)r | ((uint32_t)g << 8) | ((uint32_t)b << 16);
}

/// shade() for a caller that has already checked distSq < radiusSq.
[[gnu::always_inline]] inline int32_t shadeInRange(const ObjectLight& light, int32_t distSq, int32_t nd) {
    if (nd < 0) return 0;
    // 1024 * distSq / radiusSq, under 1024 since distSq < radiusSq; the
    // shifted distSq stays under radiusSq << tShift <= 2^32.
    uint32_t bin = mulhi((uint32_t)distSq << light.tShift, light.invRadiusSq) >> 10;
    if (bin < kExactBins) return termExact(light, distSq, nd);
    // q * k is cos * (1 - d/r) <= 4096; the table cannot push it past that by
    // more than its rounding, so there is no clamp.
    int32_t q = (int32_t)mulhi((uint32_t)nd, light.invRadius);  // cos * d / r, 4.12
    return (q * (int32_t)kFalloffLut.k[bin]) >> 10;
}

[[gnu::always_inline]] inline int32_t shade(const ObjectLight& light, int32_t distSq, int32_t nd) {
    if (distSq >= light.radiusSq) return 0;
    return shadeInRange(light, distSq, nd);
}

inline int32_t term(const ObjectLight& light, int32_t lDotV, int32_t vLenSq, int32_t nDotL,
                    int32_t nDotV) {
    return shade(light, light.lenSq - 2 * lDotV + vLenSq, nDotL - nDotV);
}

/// Saturating add of a packed 0x00BBGGRR (each byte <= 255) to the colour
/// bytes of a packed psyqo::Color, keeping its top byte. A plain add is right
/// unless a byte carries into the next, which the xor test catches; only then
/// does it take the per-byte saturating path.
inline uint32_t addClampPackedSlow(uint32_t c, uint32_t p) {
    const uint32_t lo7 = 0x7F7F7F, hi1 = 0x808080;
    uint32_t s = ((c & lo7) + (p & lo7)) ^ ((c ^ p) & hi1);       // bytewise sum mod 256
    uint32_t carry = ((c & p) | ((c | p) & ~s)) & hi1;            // carry out of each byte
    carry >>= 7;
    return ((s | ((carry << 8) - carry)) & 0xFFFFFF) | (c & 0xFF000000);
}

[[gnu::always_inline]] inline uint32_t addClampPacked(uint32_t c, uint32_t p) {
    uint32_t sum = c + p;
    if (((c ^ p ^ sum) & 0x01010100) == 0) return sum;
    return addClampPackedSlow(c, p);
}

/// Flat shading: one term per light at the triangle's first vertex, as a
/// packed 0x00BBGGRR to add to all three vertex colours, or 0 when no light
/// reaches it. Positions are the raw int16 vertex coordinates, the normal is
/// 4.12. This is the CPU statement of what applyPointLightsFlat() in
/// lightgte.hh computes on the GTE; the two must agree exactly.
inline uint32_t shadeFlat(const ObjectLights& ol, const int16_t x[3], const int16_t y[3],
                          const int16_t z[3], int32_t nx, int32_t ny, int32_t nz) {
    uint32_t p = 0;
    for (int i = 0; i < ol.count; i++) {
        const ObjectLight& l = ol.lights[i];
        const int32_t dx = flatDelta(ol, l.lx, x[0]), dy = flatDelta(ol, l.ly, y[0]),
                      dz = flatDelta(ol, l.lz, z[0]);
        int32_t t = shade(l, dx * dx + dy * dy + dz * dz, nx * dx + ny * dy + nz * dz);
        if (t == 0) continue;
        p = addClampPacked(p, flatColour(l.cr, l.cg, l.cb, t));
    }
    return p;
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
