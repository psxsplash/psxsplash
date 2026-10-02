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

/// Add the lights loaded by loadLightMatrices() to a triangle's three vertex
/// colours. Touches V0, IR and MAC only, so the rotation and translation the
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
