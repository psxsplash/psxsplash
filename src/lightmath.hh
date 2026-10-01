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

/// Falloff times cosine for one vertex and one light, 4.12 (0..4096), or 0 if
/// the vertex is out of range or faces away. The dot products are the ones the
/// renderer gets from the GTE, all on shifted operands:
///   lDotV = L.v, vLenSq = v.v, nDotL = N.L, nDotV = N.v
/// with N the 4.12 normal and L, v the shifted light and vertex positions.
int32_t term(const ObjectLight& light, int32_t lDotV, int32_t vLenSq, int32_t nDotL,
             int32_t nDotV);

/// baked + add, clamped to a colour byte.
inline uint8_t addClamp(uint8_t baked, int32_t add) {
    int32_t v = baked + add;
    return v > 255 ? 255 : (uint8_t)v;
}

}  // namespace lightmath
}  // namespace psxsplash
