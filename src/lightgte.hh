#pragma once

#include <psyqo/gte-kernels.hh>
#include <psyqo/gte-registers.hh>
#include <psyqo/matrix.hh>
#include <psyqo/primitives/common.hh>

#include "lightmath.hh"
#include "mesh.hh"

// The GTE half of the point lights: the dot products lightmath::term() needs.
// Split from renderer.cpp so a standalone program can run it on the GTE and
// compare against the host-tested lightmath.

namespace psxsplash {

/// Light positions go in as matrix rows, so one MVMVA dots a vector against
/// lights 0-2 (light matrix) and a second against light 3 (colour matrix).
/// Nothing else in the engine uses either matrix.
inline void loadLightMatrices(const lightmath::ObjectLights& ol) {
    psyqo::Matrix33 ll = {}, lc = {};
    for (int i = 0; i < ol.count && i < 3; i++) {
        ll.vs[i].x.value = ol.lights[i].lx;
        ll.vs[i].y.value = ol.lights[i].ly;
        ll.vs[i].z.value = ol.lights[i].lz;
    }
    if (ol.count > 3) {
        lc.vs[0].x.value = ol.lights[3].lx;
        lc.vs[0].y.value = ol.lights[3].ly;
        lc.vs[0].z.value = ol.lights[3].lz;
    }
    psyqo::GTE::writeSafe<psyqo::GTE::PseudoRegister::Light>(ll);
    psyqo::GTE::writeSafe<psyqo::GTE::PseudoRegister::Color>(lc);
}

/// out[0..2] = L0-2 . (x,y,z), and with four lights out[3] = L3 . (x,y,z).
/// Shift factor off, so the sums are exact.
inline void dotLights(int32_t x, int32_t y, int32_t z, int count, int32_t out[4]) {
    using namespace psyqo::GTE;
    write<Register::VXY0, Unsafe>((uint32_t)(x & 0xFFFF) | ((uint32_t)y << 16));
    write<Register::VZ0, Safe>((uint32_t)z);
    Kernels::mvmva<Kernels::MX::LL, Kernels::MV::V0, Kernels::TV::Zero, Kernels::Unshifted>();
    out[0] = (int32_t)readRaw<Register::MAC1, Safe>();
    out[1] = (int32_t)readRaw<Register::MAC2, Safe>();
    out[2] = (int32_t)readRaw<Register::MAC3, Safe>();
    if (count > 3) {
        Kernels::mvmva<Kernels::MX::LC, Kernels::MV::V0, Kernels::TV::Zero, Kernels::Unshifted>();
        out[3] = (int32_t)readRaw<Register::MAC1, Safe>();
    }
}

// Raw GTE moves for the flat path. psyqo's write<>/readRaw<> are not always
// inlined at -Os, and a call per register move costs more than the lighting.
// Writes end with the two instructions the GTE needs before a command reads
// them; reads carry the one-instruction load delay.
[[gnu::always_inline]] inline void gteMtc2V0(uint32_t xy, uint32_t z) {
    asm volatile(".set push\n\t.set noreorder\n\tmtc2 %0, $0\n\tmtc2 %1, $1\n\t.set pop" : : "r"(xy), "r"(z));
}
[[gnu::always_inline]] inline void gteSetBK(uint32_t x, uint32_t y, uint32_t z) {
    asm volatile("ctc2 %0, $13\n\tctc2 %1, $14\n\tctc2 %2, $15\n\tnop\n\tnop" : : "r"(x), "r"(y), "r"(z));
}
[[gnu::always_inline]] inline void gteSetL1(uint32_t xy, uint32_t z) {
    asm volatile("ctc2 %0, $8\n\tctc2 %1, $9\n\tnop\n\tnop" : : "r"(xy), "r"(z));
}
/// MAC1 + MAC2 + MAC3: three reads sharing one load-delay slot.
[[gnu::always_inline]] inline int32_t gteMacSum() {
    int32_t a, b, c;
    asm volatile(".set push\n\t.set noreorder\n\tmfc2 %0, $25\n\tmfc2 %1, $26\n\tmfc2 %2, $27\n\tnop\n\t.set pop"
                 : "=&r"(a), "=&r"(b), "=&r"(c));
    return a + b + c;
}
[[gnu::always_inline]] inline int32_t gteMac(int which) {
    int32_t v;
    if (which == 1) asm volatile("mfc2 %0, $25\n\tnop" : "=r"(v));
    else if (which == 2) asm volatile("mfc2 %0, $26\n\tnop" : "=r"(v));
    else asm volatile("mfc2 %0, $27\n\tnop" : "=r"(v));
    return v;
}

/// GTE state for applyPointLightsFlat(): the colour matrix is -I scaled by the
/// object's shift, so an MVMVA with the background colour vector as translation
/// computes BK - V0 >> shift from a raw vertex in V0, and the
/// light matrix's other two rows are zero (its first row takes each triangle's
/// normal). BK holds the light; with a single light it is loaded here, once per
/// object.
inline void loadFlatLightState(const lightmath::ObjectLights& ol) {
    using namespace psyqo::GTE;
    psyqo::Matrix33 negI = {}, zero = {};
    negI.vs[0].x.value = ol.flatScale;
    negI.vs[1].y.value = ol.flatScale;
    negI.vs[2].z.value = ol.flatScale;
    writeSafe<PseudoRegister::Color>(negI);
    writeSafe<PseudoRegister::Light>(zero);
    if (ol.count == 1) gteSetBK(ol.lights[0].lx, ol.lights[0].ly, ol.lights[0].lz);
}

/// Flat point lighting: one colour for the whole triangle, evaluated at its
/// first vertex and added to all three baked vertex colours. Per light, on the
/// GTE: d = light - v0 (MVMVA, colour matrix, BK), d*d (SQR), and N.d (MVMVA,
/// light matrix row 0 = normal). Matches lightmath::shadeFlat() exactly.
/// The first vertex rather than the centroid: the centroid costs a fifth of
/// the whole path, and v0 goes into the GTE straight from the triangle.
[[gnu::always_inline]] inline void applyPointLightsFlat(const lightmath::ObjectLights& ol, const Tri& tri,
                                                        psyqo::Color& cA, psyqo::Color& cB, psyqo::Color& cC) {
    using namespace psyqo::GTE;
    // Tri is 4-aligned and v0 sits at offset 0, so x and y load as one word.
    gteMtc2V0(*reinterpret_cast<const uint32_t*>(&tri.v0), (uint32_t)(int32_t)tri.v0.z.value);
    const uint32_t nxy = (uint32_t)(uint16_t)tri.normal.x.value | ((uint32_t)tri.normal.y.value << 16);
    const uint32_t nz = (uint32_t)(uint16_t)tri.normal.z.value;

    if (ol.count == 1) {
        // The common case, without the loop: BK already holds the light.
        const lightmath::ObjectLight& l = ol.lights[0];
        Kernels::mvmva<Kernels::MX::LC, Kernels::MV::V0, Kernels::TV::BK, Kernels::Shifted>();
        Kernels::sqr<Kernels::Unshifted>();
        int32_t distSq = gteMacSum();
        if (distSq >= l.radiusSq) return;
        // Out-of-range triangles, often half of a lit mesh, stop above this line.
        gteSetL1(nxy, nz);
        Kernels::mvmva<Kernels::MX::LC, Kernels::MV::V0, Kernels::TV::BK, Kernels::Shifted>();
        Kernels::mvmva<Kernels::MX::LL, Kernels::MV::IR, Kernels::TV::Zero, Kernels::Unshifted>();
        int32_t t = lightmath::shadeInRange(l, distSq, gteMac(1));
        if (t == 0) return;
        const uint32_t p = l.colourLut[t >> lightmath::kColourLutShift];
        cA.packed = lightmath::addClampPacked(cA.packed, p);
        cB.packed = lightmath::addClampPacked(cB.packed, p);
        cC.packed = lightmath::addClampPacked(cC.packed, p);
        return;
    }

    // Each light's colour comes out of its table (lightmath::flatColour), and
    // lights combine with the same saturating add as the baked colour.
    gteSetL1(nxy, nz);
    uint32_t p = 0;
    for (int i = 0; i < ol.count; i++) {
        const lightmath::ObjectLight& l = ol.lights[i];
        if (ol.count > 1) gteSetBK(l.lx, l.ly, l.lz);
        Kernels::mvmva<Kernels::MX::LC, Kernels::MV::V0, Kernels::TV::BK, Kernels::Shifted>();
        Kernels::sqr<Kernels::Unshifted>();
        int32_t distSq = gteMacSum();
        if (distSq >= l.radiusSq) continue;
        // SQR consumed IR; rebuild d there and dot it against the normal.
        Kernels::mvmva<Kernels::MX::LC, Kernels::MV::V0, Kernels::TV::BK, Kernels::Shifted>();
        Kernels::mvmva<Kernels::MX::LL, Kernels::MV::IR, Kernels::TV::Zero, Kernels::Unshifted>();
        int32_t t = lightmath::shade(l, distSq, gteMac(1));
        if (t == 0) continue;
        p = lightmath::addClampPacked(p, l.colourLut[t >> lightmath::kColourLutShift]);
    }
    if (p == 0) return;
    cA.packed = lightmath::addClampPacked(cA.packed, p);
    cB.packed = lightmath::addClampPacked(cB.packed, p);
    cC.packed = lightmath::addClampPacked(cC.packed, p);
}

/// Smooth point lighting, per vertex: add the lights loaded by
/// loadLightMatrices() to a triangle's three vertex colours. Touches V0, IR and MAC only, so the rotation and translation the
/// next triangle projects with are left alone.
inline void applyPointLights(const lightmath::ObjectLights& ol, const Tri& tri, psyqo::Color& cA,
                             psyqo::Color& cB, psyqo::Color& cC) {
    const int n = ol.count;
    const int shift = ol.shift;

    const psyqo::GTE::PackedVec3* verts[3] = {&tri.v0, &tri.v1, &tri.v2};
    int32_t v[3][3];
    for (int vi = 0; vi < 3; vi++) {
        v[vi][0] = lightmath::shiftRound(verts[vi]->x.value, shift);
        v[vi][1] = lightmath::shiftRound(verts[vi]->y.value, shift);
        v[vi][2] = lightmath::shiftRound(verts[vi]->z.value, shift);
    }
    // Most triangles of a lit mesh sit outside every light: skip them before
    // touching the GTE.
    const uint32_t mask = lightmath::reachMask(ol, v);
    if (mask == 0) return;

    const int32_t nx = tri.normal.x.value, ny = tri.normal.y.value, nz = tri.normal.z.value;
    int32_t nDotL[4];
    dotLights(nx, ny, nz, n, nDotL);

    psyqo::Color* colors[3] = {&cA, &cB, &cC};
    for (int vi = 0; vi < 3; vi++) {
        const int32_t vx = v[vi][0], vy = v[vi][1], vz = v[vi][2];
        int32_t lDotV[4];
        dotLights(vx, vy, vz, n, lDotV);
        int32_t vLenSq = vx * vx + vy * vy + vz * vz;
        int32_t nDotV = nx * vx + ny * vy + nz * vz;

        int32_t addR = 0, addG = 0, addB = 0;
        for (int i = 0; i < n; i++) {
            if (!(mask & (1u << i))) continue;
            const lightmath::ObjectLight& l = ol.lights[i];
            int32_t t = lightmath::term(l, lDotV[i], vLenSq, nDotL[i], nDotV);
            if (t == 0) continue;
            addR += (l.cr * t) >> 12;
            addG += (l.cg * t) >> 12;
            addB += (l.cb * t) >> 12;
        }
        psyqo::Color& c = *colors[vi];
        c.r = lightmath::addClamp(c.r, addR);
        c.g = lightmath::addClamp(c.g, addG);
        c.b = lightmath::addClamp(c.b, addB);
    }
}

}  // namespace psxsplash
