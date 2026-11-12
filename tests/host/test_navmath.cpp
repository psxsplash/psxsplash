// Host tests for nav region geometry (src/navmath.hh), compiled natively.
//
// The clamp must project onto an edge however short it is, and a player
// pressed against a wall that runs on across a fan of regions must slide past
// the fan's vertex instead of stopping on it.

#include <math.h>
#include <stdint.h>

#include "navmath.hh"

#include "testing.hh"

using namespace psxsplash;
using namespace psxsplash::navmath;

namespace {

struct Region {
    int32_t vertsX[8];
    int32_t vertsZ[8];
    uint16_t portalStart;
    uint8_t portalCount;
    uint8_t vertCount;
    uint8_t walkoffEdgeMask;
};

struct Portal {
    uint16_t neighborRegion;
};

// Four regions of the courtyard example's navmesh around the vertex (270, 168),
// as the engine loaded them (20.12): a wall runs from (248, 168) to (270, 168)
// on region 0, then on to (285, 178) on region 3, three portals away.
const Region kFan[] = {
    {{270, 248, 244}, {168, 168, 154}, 0, 1, 3, 0},
    {{270, 244, 229}, {168, 154, 143}, 1, 2, 3, 0},
    {{270, 229, 43, 29, 348}, {168, 143, 14, 4, -61}, 3, 2, 5, 0},
    {{348, 285, 270}, {-61, 178, 168}, 5, 1, 3, 0},
};
const Portal kFanPortals[] = {{1}, {0}, {2}, {1}, {3}, {2}};

}  // namespace

TEST(projection_keeps_short_edges) {
    // Every edge from 1 to 300 units long, at several angles, points on both sides.
    for (int len = 1; len <= 300; len += 7)
        for (int a = 0; a < 16; a++) {
            double ang = a * 3.14159265358979 / 8;
            int32_t ax = 100, az = -50;
            int32_t bx = ax + (int32_t)lround(len * cos(ang)), bz = az + (int32_t)lround(len * sin(ang));
            for (int p = 0; p < 8; p++) {
                int32_t px = ax + (p * 37 % 23) - 11 + (bx - ax) * p / 7, pz = az + (p * 53 % 19) - 9 + (bz - az) * p / 7;
                int32_t cx, cz;
                closestPointOnSegment(px, pz, ax, az, bx, bz, cx, cz);
                double dx = bx - ax, dz = bz - az, L = dx * dx + dz * dz;
                double t = L == 0 ? 0 : ((px - ax) * dx + (pz - az) * dz) / L;
                t = t < 0 ? 0 : t > 1 ? 1 : t;
                double rx = ax + t * dx, rz = az + t * dz;
                if (!CHECK(fabs(cx - rx) <= 1.0 && fabs(cz - rz) <= 1.0)) return;
            }
        }
}

TEST(projection_handles_long_edges) {
    // A 4,000,000-unit edge (a kilometre at GTE scaling 1): t keeps 15 bits, so the
    // point may sit up to |ab| / 2^14 along the edge from the exact one, never off it.
    int32_t cx, cz;
    closestPointOnSegment(0, 1000000, -2000000, 0, 2000000, 0, cx, cz);
    CHECK(cx >= -245 && cx <= 245);
    CHECK_EQ(cz, 0);
    closestPointOnSegment(1500000, 1500000, 0, 0, 2000000, 2000000, cx, cz);
    CHECK(cx >= 1500000 - 175 && cx <= 1500000 + 175);
    CHECK_EQ(cx, cz);
}

TEST(sliding_along_a_wall_passes_a_fan_vertex) {
    // Standing on the vertex in region 0 and stepping (+2, +3): off every region.
    int32_t x = 272, z = 171;
    for (const Region& r : kFan) CHECK(!pointInConvexPoly(x, z, r.vertsX, r.vertsZ, r.vertCount));
    clampNearby(x, z, 0, false, kFan, 4, kFanPortals, 6);
    // Onto the wall (270, 168) -> (285, 178), past the vertex.
    CHECK(x > 270);
    CHECK(llabs((int64_t)(x - 270) * 10 - (int64_t)(z - 168) * 15) <= 15);
    CHECK(pointInConvexPoly(x, z, kFan[3].vertsX, kFan[3].vertsZ, 3));
}

TEST(inside_is_left_alone) {
    int32_t x = 255, z = 165;
    clampNearby(x, z, 0, false, kFan, 4, kFanPortals, 6);
    CHECK_EQ(x, 255);
    CHECK_EQ(z, 165);
}

TEST(selective_lets_a_walkoff_edge_go) {
    Region fan[4];
    for (int i = 0; i < 4; i++) fan[i] = kFan[i];
    fan[3].walkoffEdgeMask = 1 << 1;  // (285, 178) -> (270, 168)
    int32_t x = 272, z = 171;
    clampNearby(x, z, 0, true, fan, 4, kFanPortals, 6);
    CHECK_EQ(x, 272);
    CHECK_EQ(z, 171);
    clampNearby(x, z, 0, false, fan, 4, kFanPortals, 6);
    CHECK(x != 272 || z != 171);
}

int main() { return psxsplash::test::runAll(); }
