// Host tests for the point-light math (src/lightmath.cpp), compiled natively.
//
// The renderer gets its dot products from the GTE (MVMVA with the shift factor
// off, which is exact integer arithmetic), so computing them here with plain
// integer multiplies feeds lightmath the same numbers the console does.

#include <math.h>
#include <stdlib.h>

#include "lightmath.hh"

#include "testing.hh"

using namespace psxsplash;
using namespace psxsplash::lightmath;

namespace {

const int32_t kIdentity[3][3] = {{4096, 0, 0}, {0, 4096, 0}, {0, 0, 4096}};

PointLight makeLight(int32_t x, int32_t y, int32_t z, int32_t radius) {
    PointLight l{};
    l.x = x;
    l.y = y;
    l.z = z;
    l.radius = radius;
    l.intensity = 4096;
    l.r = 255;
    l.g = 128;
    l.b = 0;
    l.enabled = 1;
    return l;
}

// What renderer.cpp does per vertex, with the GTE dot products done on the CPU.
int32_t termFor(const ObjectLights& ol, int i, const int16_t v[3], const int16_t n[3]) {
    const ObjectLight& l = ol.lights[i];
    int32_t vx = shiftRound(v[0], ol.shift), vy = shiftRound(v[1], ol.shift),
            vz = shiftRound(v[2], ol.shift);
    int32_t lDotV = l.lx * vx + l.ly * vy + l.lz * vz;
    int32_t vLenSq = vx * vx + vy * vy + vz * vz;
    int32_t nDotL = n[0] * l.lx + n[1] * l.ly + n[2] * l.lz;
    int32_t nDotV = n[0] * vx + n[1] * vy + n[2] * vz;
    return term(l, lDotV, vLenSq, nDotL, nDotV);
}

// The model the integer path approximates, in floating point.
double reference(const PointLight& light, const int32_t pos[3], const int16_t v[3],
                 const int16_t n[3]) {
    double dx = (light.x - pos[0] - v[0]) / 4096.0;
    double dy = (light.y - pos[1] - v[1]) / 4096.0;
    double dz = (light.z - pos[2] - v[2]) / 4096.0;
    double d = sqrt(dx * dx + dy * dy + dz * dz);
    double r = light.radius / 4096.0;
    if (d >= r || d == 0.0) return 0.0;
    double nl = (n[0] * dx + n[1] * dy + n[2] * dz) / (4096.0 * d);
    if (nl <= 0.0) return 0.0;
    return 4096.0 * (1.0 - d / r) * nl;
}

const int32_t kOrigin[3] = {0, 0, 0};
const int32_t kBoxMin[3] = {-8 * 4096, -8 * 4096, -8 * 4096};
const int32_t kBoxMax[3] = {8 * 4096, 8 * 4096, 8 * 4096};

}  // namespace

TEST(sphere_aabb_inside_touching_and_outside) {
    const int32_t mn[3] = {0, 0, 0};
    const int32_t mx[3] = {4096, 4096, 4096};
    CHECK(sphereTouchesAABB(2048, 2048, 2048, 1, mn, mx));          // centre inside
    CHECK(sphereTouchesAABB(4096 + 100, 2048, 2048, 101, mn, mx));  // just reaches a face
    CHECK(!sphereTouchesAABB(4096 + 100, 2048, 2048, 100, mn, mx)); // stops at the face
    // Off a corner: per-axis distance 1000 each, true distance ~1732.
    CHECK(!sphereTouchesAABB(5096, 5096, 5096, 1700, mn, mx));
    CHECK(sphereTouchesAABB(5096, 5096, 5096, 1740, mn, mx));
    CHECK(!sphereTouchesAABB(2048, 2048, 2048, 0, mn, mx));         // zero radius is off
}

TEST(prepare_skips_disabled_and_out_of_range_and_caps_at_four) {
    PointLight lights[7];
    for (int i = 0; i < 7; i++) lights[i] = makeLight(i * 4096, 0, 0, 4 * 4096);
    lights[1].enabled = 0;
    lights[2].x = 100 * 4096;  // far outside the box
    ObjectLights ol;
    const int32_t mn[3] = {-4096, -4096, -4096};
    const int32_t mx[3] = {4 * 4096, 4096, 4096};
    CHECK_EQ(prepare(lights, 7, kOrigin, kIdentity, mn, mx, ol), MAX_LIGHTS_PER_MESH);
    // Kept in scene order: 0, 3, 4, 5. Light 6 is in range but over the cap.
    CHECK_EQ(ol.lights[0].lx, 0);
    CHECK_EQ(ol.lights[1].lx, shiftRound(3 * 4096, ol.shift));
    CHECK_EQ(ol.lights[3].lx, shiftRound(5 * 4096, ol.shift));
}

TEST(prepare_no_lights_returns_zero) {
    ObjectLights ol;
    CHECK_EQ(prepare(nullptr, 0, kOrigin, kIdentity, kBoxMin, kBoxMax, ol), 0);
    CHECK_EQ(ol.count, 0);
}

TEST(prepare_moves_light_into_object_space) {
    // Object at (10,0,0) rotated 90 degrees about Y: local +X maps to world -Z.
    // rows of rot: world = rot * local.
    const int32_t rot[3][3] = {{0, 0, 4096}, {0, 4096, 0}, {-4096, 0, 0}};
    const int32_t pos[3] = {10 * 4096, 0, 0};
    const int32_t mn[3] = {8 * 4096, -4096, -4096};
    const int32_t mx[3] = {12 * 4096, 4096, 4096};
    // Light one unit along world -Z from the object: local +X.
    PointLight l = makeLight(10 * 4096, 0, -4096, 4 * 4096);
    ObjectLights ol;
    CHECK_EQ(prepare(&l, 1, pos, rot, mn, mx, ol), 1);
    CHECK_EQ(ol.lights[0].lx, shiftRound(4096, ol.shift));
    CHECK_EQ(ol.lights[0].ly, 0);
    CHECK_EQ(ol.lights[0].lz, 0);
}

TEST(term_full_strength_at_light_and_zero_at_radius) {
    PointLight l = makeLight(0, -2 * 4096, 0, 4 * 4096);  // two units "up" (y is down)
    ObjectLights ol;
    CHECK_EQ(prepare(&l, 1, kOrigin, kIdentity, kBoxMin, kBoxMax, ol), 1);
    const int16_t up[3] = {0, -4096, 0};
    const int16_t atLight[3] = {0, -2 * 4096, 0};
    const int16_t pastRadius[3] = {0, 3 * 4096, 0};  // 5 units away
    CHECK_EQ(termFor(ol, 0, atLight, up), 4096);
    CHECK_EQ(termFor(ol, 0, pastRadius, up), 0);
}

TEST(term_zero_when_facing_away) {
    PointLight l = makeLight(0, -2 * 4096, 0, 4 * 4096);
    ObjectLights ol;
    prepare(&l, 1, kOrigin, kIdentity, kBoxMin, kBoxMax, ol);
    const int16_t down[3] = {0, 4096, 0};
    const int16_t v[3] = {0, 0, 0};
    CHECK_EQ(termFor(ol, 0, v, down), 0);
}

TEST(term_tracks_float_reference) {
    // Random lights and vertices across the int16 vertex range and radii from
    // half a unit to sixty-four units. Reports the worst error so a regression
    // in precision shows up as a number, not just a pass.
    srand(12345);
    double worst = 0.0, sum = 0.0;
    int samples = 0, lit = 0, nearBin = 0;
    for (int trial = 0; trial < 20000; trial++) {
        int32_t radius = 2048 + rand() % (64 * 4096);
        PointLight l = makeLight((rand() % 65536) - 32768, (rand() % 65536) - 32768,
                                 (rand() % 65536) - 32768, radius);
        ObjectLights ol;
        if (prepare(&l, 1, kOrigin, kIdentity, kBoxMin, kBoxMax, ol) != 1) continue;
        int16_t v[3] = {(int16_t)((rand() % 65536) - 32768), (int16_t)((rand() % 65536) - 32768),
                        (int16_t)((rand() % 65536) - 32768)};
        // Unit normal from a random direction.
        double nx = rand() - RAND_MAX / 2.0, ny = rand() - RAND_MAX / 2.0, nz = rand() - RAND_MAX / 2.0;
        double nlen = sqrt(nx * nx + ny * ny + nz * nz);
        int16_t n[3] = {(int16_t)lround(4096 * nx / nlen), (int16_t)lround(4096 * ny / nlen),
                        (int16_t)lround(4096 * nz / nlen)};
        double ref = reference(l, kOrigin, v, n);
        int32_t got = termFor(ol, 0, v, n);
        samples++;
        if (ref > 0.0 || got > 0) lit++;
        double dx = l.x - v[0], dy = l.y - v[1], dz = l.z - v[2];
        if (sqrt(dx * dx + dy * dy + dz * dz) < radius / 4.0) nearBin++;
        double err = fabs(got - ref);
        sum += err;
        if (err > worst) worst = err;
    }
    printf("    light term vs float: %d samples, %d lit, %d inside r/4 (exact path), "
           "worst %.1f/4096, mean %.2f/4096\n",
           samples, lit, nearBin, worst, sum / samples);
    CHECK(lit > 1000);
    CHECK(worst < 4096 * 0.02);
}

TEST(term_near_the_light_tracks_float_reference) {
    // The exact path (d < r/4), sampled densely: random scatter above lands
    // there about one time in sixty-four.
    srand(777);
    double worst = 0.0;
    int samples = 0, skipped = 0;
    for (int trial = 0; trial < 20000; trial++) {
        int32_t radius = 4096 + rand() % (32 * 4096);
        PointLight l = makeLight(0, 0, 0, radius);
        ObjectLights ol;
        if (prepare(&l, 1, kOrigin, kIdentity, kBoxMin, kBoxMax, ol) != 1) continue;
        double ux = rand() - RAND_MAX / 2.0, uy = rand() - RAND_MAX / 2.0, uz = rand() - RAND_MAX / 2.0;
        double ulen = sqrt(ux * ux + uy * uy + uz * uz);
        double d = (rand() / (double)RAND_MAX) * radius / 4.0;
        if (d > 32767) d = 32767;
        int16_t v[3] = {(int16_t)lround(ux / ulen * d), (int16_t)lround(uy / ulen * d),
                        (int16_t)lround(uz / ulen * d)};
        double nx = rand() - RAND_MAX / 2.0, ny = rand() - RAND_MAX / 2.0, nz = rand() - RAND_MAX / 2.0;
        double nlen = sqrt(nx * nx + ny * ny + nz * nz);
        int16_t n[3] = {(int16_t)lround(4096 * nx / nlen), (int16_t)lround(4096 * ny / nlen),
                        (int16_t)lround(4096 * nz / nlen)};
        double err = fabs(termFor(ol, 0, v, n) - reference(l, kOrigin, v, n));
        // Positions are quantised to 1 << shift, so within a few dozen of those
        // steps of the light the direction to it is coarse. Measured: 1% error at
        // 20 steps, 12% at 4. Grade outside 32 steps and count what was skipped.
        if (d < 32.0 * (1 << ol.shift)) {
            skipped++;
            continue;
        }
        samples++;
        if (err > worst) worst = err;
    }
    printf("    near light: %d samples, %d within 32 steps skipped, worst %.1f/4096\n", samples,
           skipped, worst);
    CHECK(samples > 10000);
    CHECK(worst < 4096 * 0.02);
}

TEST(reach_mask_never_drops_a_lit_vertex) {
    // The renderer skips a triangle whose bit is clear, so a clear bit on a
    // triangle term() would light is a hole in the lighting.
    srand(4242);
    int lit = 0, culled = 0, holes = 0;
    for (int trial = 0; trial < 20000; trial++) {
        PointLight l = makeLight((rand() % 40000) - 20000, (rand() % 40000) - 20000,
                                 (rand() % 40000) - 20000, 2048 + rand() % (8 * 4096));
        ObjectLights ol;
        if (prepare(&l, 1, kOrigin, kIdentity, kBoxMin, kBoxMax, ol) != 1) continue;
        int16_t base[3] = {(int16_t)((rand() % 50000) - 25000), (int16_t)((rand() % 50000) - 25000),
                           (int16_t)((rand() % 50000) - 25000)};
        int16_t vs[3][3];
        int32_t sv[3][3];
        for (int k = 0; k < 3; k++)
            for (int a = 0; a < 3; a++) {
                vs[k][a] = (int16_t)(base[a] + (rand() % 8000) - 4000);
                sv[k][a] = shiftRound(vs[k][a], ol.shift);
            }
        const int16_t n[3] = {0, -4096, 0};
        bool anyLit = false;
        for (int k = 0; k < 3; k++)
            if (termFor(ol, 0, vs[k], n) > 0) anyLit = true;
        uint32_t mask = reachMask(ol, sv);
        if (anyLit) lit++;
        if (!mask) culled++;
        if (anyLit && !(mask & 1)) holes++;
    }
    printf("    reach mask: %d lit, %d culled, %d holes\n", lit, culled, holes);
    CHECK(lit > 500);
    CHECK(culled > 1000);
    CHECK_EQ(holes, 0);
}

TEST(add_clamp_saturates) {
    CHECK_EQ(addClamp(200, 30), 230);
    CHECK_EQ(addClamp(200, 100), 255);
    CHECK_EQ(addClamp(0, 0), 0);
}

int main() { return psxsplash::test::runAll(); }
