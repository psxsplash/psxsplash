#include "navmesh.hh"

#include <array>

#include <psyqo/fixed-point.hh>
#include <psyqo/vector.hh>

using namespace psyqo::fixed_point_literals;

// FIXME: This entire file uses hard FixedPoint scaling of 100. This is not ideal.
// It would be better to move the fixedpoint precision to 19 instead.

namespace psxsplash {

psyqo::FixedPoint<12> DotProduct2D(const psyqo::Vec2& a, const psyqo::Vec2& b) { return a.x * b.x + a.y * b.y; }

psyqo::Vec2 ClosestPointOnSegment(const psyqo::Vec2& A, const psyqo::Vec2& B, const psyqo::Vec2& P) {
    psyqo::Vec2 AB = {B.x - A.x, B.y - A.y};
    psyqo::Vec2 AP = {P.x - A.x, P.y - A.y};
    auto abDot = DotProduct2D(AB, AB);
    if (abDot == 0) return A;
    psyqo::FixedPoint<12> t = DotProduct2D(AP, AB) / abDot;
    if (t < 0.0_fp) t = 0.0_fp;
    if (t > 1.0_fp) t = 1.0_fp;
    return {(A.x + AB.x * t), (A.y + AB.y * t)};
}

bool PointInTriangle(psyqo::Vec3& p, NavMeshTri& tri) {
    psyqo::Vec2 A = {tri.v0.x * 100, tri.v0.z * 100};
    psyqo::Vec2 B = {tri.v1.x * 100, tri.v1.z * 100};
    psyqo::Vec2 C = {tri.v2.x * 100, tri.v2.z * 100};
    psyqo::Vec2 P = {p.x * 100, p.z * 100};

    psyqo::Vec2 v0 = {B.x - A.x, B.y - A.y};
    psyqo::Vec2 v1 = {C.x - A.x, C.y - A.y};
    psyqo::Vec2 v2 = {P.x - A.x, P.y - A.y};

    auto d00 = DotProduct2D(v0, v0);
    auto d01 = DotProduct2D(v0, v1);
    auto d11 = DotProduct2D(v1, v1);
    auto d20 = DotProduct2D(v2, v0);
    auto d21 = DotProduct2D(v2, v1);

    psyqo::FixedPoint<12> denom = d00 * d11 - d01 * d01;
    if (denom == 0.0_fp) {
        return false;
    }
    auto invDenom = 1.0_fp / denom;
    auto u = (d11 * d20 - d01 * d21) * invDenom;
    auto w = (d00 * d21 - d01 * d20) * invDenom;

    return (u >= 0.0_fp) && (w >= 0.0_fp) && (u + w <= 1.0_fp);
}

psyqo::Vec3 ComputeNormal(const NavMeshTri& tri) {
    psyqo::Vec3 v1 = {tri.v1.x * 100 - tri.v0.x  * 100, tri.v1.y  * 100 - tri.v0.y  * 100, tri.v1.z  * 100 - tri.v0.z  * 100};
    psyqo::Vec3 v2 = {tri.v2.x  * 100 - tri.v0.x  * 100, tri.v2.y  * 100 - tri.v0.y  * 100, tri.v2.z  * 100 - tri.v0.z  * 100};

    psyqo::Vec3 normal = {
        v1.y * v2.z - v1.z * v2.y,
        v1.z * v2.x - v1.x * v2.z,
        v1.x * v2.y - v1.y * v2.x
    };
    return normal;
}

psyqo::FixedPoint<12> CalculateY(const psyqo::Vec3& p, const NavMeshTri& tri) {
    psyqo::Vec3 normal = ComputeNormal(tri);

    psyqo::FixedPoint<12> A = normal.x;
    psyqo::FixedPoint<12> B = normal.y;
    psyqo::FixedPoint<12> C = normal.z;

    psyqo::FixedPoint<12> D = -(A * tri.v0.x + B * tri.v0.y + C * tri.v0.z);

    if (B != 0.0_fp) {
        return -(A * p.x + C * p.z + D) / B;
    } else {
        return p.y;
    }
}

psyqo::Vec3 ComputeNavmeshPosition(psyqo::Vec3& position, Navmesh& navmesh, psyqo::FixedPoint<12> playerHeight) {
    for (int i = 0; i < navmesh.triangleCount; i++) {
        if (PointInTriangle(position, navmesh.polygons[i])) {
            position.y = CalculateY(position, navmesh.polygons[i]) - playerHeight;
            return position;
        }
    }

    psyqo::Vec2 P = {position.x * 100, position.z * 100};

    psyqo::Vec2 closestPoint;
    psyqo::FixedPoint<12> minDist = 0x7ffff;

    for (int i = 0; i < navmesh.triangleCount; i++) {
        NavMeshTri& tri = navmesh.polygons[i];
        psyqo::Vec2 A = {tri.v0.x * 100, tri.v0.z * 100};
        psyqo::Vec2 B = {tri.v1.x * 100, tri.v1.z * 100};
        psyqo::Vec2 C = {tri.v2.x * 100, tri.v2.z * 100};

        std::array<std::pair<psyqo::Vec2, psyqo::Vec2>, 3> edges = {{{A, B}, {B, C}, {C, A}}};

        for (auto& edge : edges) {
            psyqo::Vec2 proj = ClosestPointOnSegment(edge.first, edge.second, P);
            psyqo::Vec2 diff = {proj.x - P.x, proj.y - P.y};
            auto distSq = DotProduct2D(diff, diff);
            if (distSq < minDist) {
                minDist = distSq;
                closestPoint = proj;
                position.y = CalculateY(position, navmesh.polygons[i]) - playerHeight;
            }
        }
    }

    position.x = closestPoint.x / 100;
    position.z = closestPoint.y / 100;

    return position;
}

}  // namespace psxsplash
