#include "worldcollision.hh"

#include <psyqo/fixed-point.hh>
#include <psyqo/vector.hh>


namespace psxsplash {

// ============================================================================
// Fixed-point helpers (20.12)
// ============================================================================

static constexpr int FRAC_BITS = 12;
static constexpr int32_t FP_ONE = 1 << FRAC_BITS;  // 4096

// Multiply two 20.12 values → 20.12
static inline int32_t fpmul(int32_t a, int32_t b) {
    return (int32_t)(((int64_t)a * b) >> FRAC_BITS);
}

// Fixed-point division: returns (a << 12) / b using only 32-bit DIV.
// Uses remainder theorem: (a * 4096) / b = (a/b)*4096 + ((a%b)*4096)/b
static inline int32_t fpdiv(int32_t a, int32_t b) {
    if (b == 0) return 0;
    int32_t q = a / b;
    int32_t r = a - q * b;
    // r * FP_ONE is safe when |r| < 524288 (which covers most game values)
    return q * FP_ONE + (r << FRAC_BITS) / b;
}

// Dot product of two 3-vectors in 20.12
static inline int32_t dot3(int32_t ax, int32_t ay, int32_t az,
                           int32_t bx, int32_t by, int32_t bz) {
    return (int32_t)((((int64_t)ax * bx) + ((int64_t)ay * by) + ((int64_t)az * bz)) >> FRAC_BITS);
}

// Cross product components (each result is 20.12)
static inline void cross3(int32_t ax, int32_t ay, int32_t az,
                           int32_t bx, int32_t by, int32_t bz,
                           int32_t& rx, int32_t& ry, int32_t& rz) {
    rx = (int32_t)(((int64_t)ay * bz - (int64_t)az * by) >> FRAC_BITS);
    ry = (int32_t)(((int64_t)az * bx - (int64_t)ax * bz) >> FRAC_BITS);
    rz = (int32_t)(((int64_t)ax * by - (int64_t)ay * bx) >> FRAC_BITS);
}

// Square root approximation via Newton's method (for 20.12 input)
static int32_t fpsqrt(int32_t x) {
    if (x <= 0) return 0;
    // Initial guess: shift right by 6 (half of 12 fractional bits, then adjust)
    int32_t guess = x;
    // Rough initial guess
    if (x > FP_ONE * 16) guess = x >> 4;
    else if (x > FP_ONE) guess = x >> 2;
    else guess = FP_ONE;

    // Newton iterations: guess = (guess + x/guess) / 2 in fixed-point
    for (int i = 0; i < 8; i++) {
        if (guess == 0) return 0;
        int32_t div = fpdiv(x, guess);
        guess = (guess + div) >> 1;
    }
    return guess;
}

// Length squared of vector (result in 20.12, but represents a squared quantity)
static inline int32_t lengthSq(int32_t x, int32_t y, int32_t z) {
    return dot3(x, y, z, x, y, z);
}

// Clamp value to [lo, hi]
static inline int32_t fpclamp(int32_t val, int32_t lo, int32_t hi) {
    if (val < lo) return lo;
    if (val > hi) return hi;
    return val;
}

// ============================================================================
// Initialization
// ============================================================================

const uint8_t* WorldCollision::initializeFromData(const uint8_t* data) {
    // Read header
    const auto* hdr = reinterpret_cast<const CollisionDataHeader*>(data);
    m_header = *hdr;
    data += sizeof(CollisionDataHeader);

    // Mesh headers
    m_meshes = reinterpret_cast<const CollisionMeshHeader*>(data);
    data += m_header.meshCount * sizeof(CollisionMeshHeader);

    // Triangles
    m_triangles = reinterpret_cast<const CollisionTri*>(data);
    data += m_header.triangleCount * sizeof(CollisionTri);

    // Spatial chunks (exterior only)
    if (m_header.chunkGridW > 0 && m_header.chunkGridH > 0) {
        m_chunks = reinterpret_cast<const CollisionChunk*>(data);
        data += m_header.chunkGridW * m_header.chunkGridH * sizeof(CollisionChunk);
    } else {
        m_chunks = nullptr;
    }


    return data;
}

// ============================================================================
// Broad phase: gather candidate meshes
// ============================================================================

int WorldCollision::gatherCandidateMeshes(int32_t posX, int32_t posZ,
                                           uint8_t currentRoom,
                                           uint16_t* outIndices,
                                           int maxIndices) const {
    int count = 0;

    if (m_chunks && m_header.chunkGridW > 0) {
        // Exterior: spatial grid lookup
        // dividing two 20.12 values gives integer grid coords directly
        int cx = 0, cz = 0;
        if (m_header.chunkSize > 0) {
            cx = (posX - m_header.chunkOriginX) / m_header.chunkSize;
            cz = (posZ - m_header.chunkOriginZ) / m_header.chunkSize;
        }

        // Check 3x3 neighborhood for robustness
        for (int dz = -1; dz <= 1 && count < maxIndices; dz++) {
            for (int dx = -1; dx <= 1 && count < maxIndices; dx++) {
                int gx = cx + dx;
                int gz = cz + dz;
                if (gx < 0 || gx >= m_header.chunkGridW || gz < 0 || gz >= m_header.chunkGridH)
                    continue;

                const auto& chunk = m_chunks[gz * m_header.chunkGridW + gx];
                for (int i = 0; i < chunk.meshCount && count < maxIndices; i++) {
                    uint16_t mi = chunk.firstMeshIndex + i;
                    if (mi < m_header.meshCount) {
                        // Deduplicate: check if already added
                        bool dup = false;
                        for (int k = 0; k < count; k++) {
                            if (outIndices[k] == mi) { dup = true; break; }
                        }
                        if (!dup) {
                            outIndices[count++] = mi;
                        }
                    }
                }
            }
        }
    } else {
        // Interior: filter by room index, or test all if no room system
        for (uint16_t i = 0; i < m_header.meshCount && count < maxIndices; i++) {
            if (currentRoom == 0xFF || m_meshes[i].roomIndex == currentRoom ||
                m_meshes[i].roomIndex == 0xFF) {
                outIndices[count++] = i;
            }
        }
    }

    return count;
}

// ============================================================================
// AABB helpers
// ============================================================================

bool WorldCollision::aabbOverlap(int32_t aMinX, int32_t aMinY, int32_t aMinZ,
                                  int32_t aMaxX, int32_t aMaxY, int32_t aMaxZ,
                                  int32_t bMinX, int32_t bMinY, int32_t bMinZ,
                                  int32_t bMaxX, int32_t bMaxY, int32_t bMaxZ) {
    return aMinX <= bMaxX && aMaxX >= bMinX &&
           aMinY <= bMaxY && aMaxY >= bMinY &&
           aMinZ <= bMaxZ && aMaxZ >= bMinZ;
}

void WorldCollision::sphereToAABB(int32_t cx, int32_t cy, int32_t cz, int32_t r,
                                   int32_t& minX, int32_t& minY, int32_t& minZ,
                                   int32_t& maxX, int32_t& maxY, int32_t& maxZ) {
    minX = cx - r; minY = cy - r; minZ = cz - r;
    maxX = cx + r; maxY = cy + r; maxZ = cz + r;
}

// ============================================================================
// Sphere vs Triangle (closest point approach)
// ============================================================================

int32_t WorldCollision::sphereVsTriangle(int32_t cx, int32_t cy, int32_t cz,
                                          int32_t radius,
                                          const CollisionTri& tri,
                                          int32_t& outNx, int32_t& outNy, int32_t& outNz) const {
    // Compute vector from v0 to sphere center
    int32_t px = cx - tri.v0x;
    int32_t py = cy - tri.v0y;
    int32_t pz = cz - tri.v0z;

    // Project onto triangle plane using precomputed normal
    int32_t dist = dot3(px, py, pz, tri.nx, tri.ny, tri.nz);

    // Quick reject if too far from plane
    int32_t absDist = dist >= 0 ? dist : -dist;
    if (absDist > radius) return 0;

    // Find closest point on triangle to sphere center
    // Use barycentric coordinates via edge projections

    // Precompute edge dot products for barycentric coords
    int32_t d00 = dot3(tri.e1x, tri.e1y, tri.e1z, tri.e1x, tri.e1y, tri.e1z);
    int32_t d01 = dot3(tri.e1x, tri.e1y, tri.e1z, tri.e2x, tri.e2y, tri.e2z);
    int32_t d11 = dot3(tri.e2x, tri.e2y, tri.e2z, tri.e2x, tri.e2y, tri.e2z);
    int32_t d20 = dot3(px, py, pz, tri.e1x, tri.e1y, tri.e1z);
    int32_t d21 = dot3(px, py, pz, tri.e2x, tri.e2y, tri.e2z);

    // Barycentric denom using fpmul (stays in 32-bit)
    int32_t denom = fpmul(d00, d11) - fpmul(d01, d01);
    if (denom == 0) return 0; // Degenerate triangle

    // Barycentric numerators (32-bit via fpmul)
    int32_t uNum = fpmul(d11, d20) - fpmul(d01, d21);
    int32_t vNum = fpmul(d00, d21) - fpmul(d01, d20);

    // u, v in 20.12 using 32-bit division only
    int32_t u = fpdiv(uNum, denom);
    int32_t v = fpdiv(vNum, denom);

    // Clamp to triangle
    int32_t w = FP_ONE - u - v;

    int32_t closestX, closestY, closestZ;

    if (u >= 0 && v >= 0 && w >= 0) {
        // Point is inside triangle — closest point is the plane projection
        closestX = cx - fpmul(dist, tri.nx);
        closestY = cy - fpmul(dist, tri.ny);
        closestZ = cz - fpmul(dist, tri.nz);
    } else {
        // Point is outside triangle — find closest point on edges/vertices
        // Check all 3 edges and pick the closest point

        // v1 = v0 + e1, v2 = v0 + e2
        int32_t v1x = tri.v0x + tri.e1x;
        int32_t v1y = tri.v0y + tri.e1y;
        int32_t v1z = tri.v0z + tri.e1z;
        int32_t v2x = tri.v0x + tri.e2x;
        int32_t v2y = tri.v0y + tri.e2y;
        int32_t v2z = tri.v0z + tri.e2z;

        int32_t bestDistSq = 0x7FFFFFFF;
        closestX = tri.v0x;
        closestY = tri.v0y;
        closestZ = tri.v0z;

        // Helper lambda: closest point on segment [A, B] to point P
        auto closestOnSeg = [&](int32_t ax, int32_t ay, int32_t az,
                                int32_t bx, int32_t by, int32_t bz,
                                int32_t& ox, int32_t& oy, int32_t& oz) {
            int32_t abx = bx - ax, aby = by - ay, abz = bz - az;
            int32_t apx = cx - ax, apy = cy - ay, apz = cz - az;
            int32_t abLen = dot3(abx, aby, abz, abx, aby, abz);
            if (abLen == 0) { ox = ax; oy = ay; oz = az; return; }
            int32_t dotAP = dot3(apx, apy, apz, abx, aby, abz);
            int32_t t = fpclamp(fpdiv(dotAP, abLen), 0, FP_ONE);
            ox = ax + fpmul(t, abx);
            oy = ay + fpmul(t, aby);
            oz = az + fpmul(t, abz);
        };

        // Edge v0→v1
        int32_t ex, ey, ez;
        closestOnSeg(tri.v0x, tri.v0y, tri.v0z, v1x, v1y, v1z, ex, ey, ez);
        int32_t dx = cx - ex, dy = cy - ey, dz = cz - ez;
        int32_t dsq = lengthSq(dx, dy, dz);
        if (dsq < bestDistSq) { bestDistSq = dsq; closestX = ex; closestY = ey; closestZ = ez; }

        // Edge v0→v2
        closestOnSeg(tri.v0x, tri.v0y, tri.v0z, v2x, v2y, v2z, ex, ey, ez);
        dx = cx - ex; dy = cy - ey; dz = cz - ez;
        dsq = lengthSq(dx, dy, dz);
        if (dsq < bestDistSq) { bestDistSq = dsq; closestX = ex; closestY = ey; closestZ = ez; }

        // Edge v1→v2
        closestOnSeg(v1x, v1y, v1z, v2x, v2y, v2z, ex, ey, ez);
        dx = cx - ex; dy = cy - ey; dz = cz - ez;
        dsq = lengthSq(dx, dy, dz);
        if (dsq < bestDistSq) { bestDistSq = dsq; closestX = ex; closestY = ey; closestZ = ez; }
    }

    // Compute vector from closest point to sphere center
    int32_t nx = cx - closestX;
    int32_t ny = cy - closestY;
    int32_t nz = cz - closestZ;

    // Use 64-bit for distance-squared comparison to avoid 20.12 underflow.
    // With small radii (e.g. radius=12 for 0.3m at GTE100), fpmul(12,12)=0
    // because 144>>12=0. This caused ALL collisions to silently fail.
    // Both sides are in the same raw scale (no shift needed for comparison).
    int64_t rawDistSq = (int64_t)nx * nx + (int64_t)ny * ny + (int64_t)nz * nz;
    int64_t rawRadSq = (int64_t)radius * radius;

    if (rawDistSq >= rawRadSq || rawDistSq == 0) return 0;

    // For the actual distance value, use fpsqrt on the 20.12 representation.
    // If the 20.12 value underflows to 0, estimate from 64-bit.
    int32_t distSq32 = (int32_t)(rawDistSq >> FRAC_BITS);
    int32_t distance;
    if (distSq32 > 0) {
        distance = fpsqrt(distSq32);
    } else {
        // Very close collision - distance is sub-unit in 20.12.
        // Use triangle normal as push direction, penetration = radius.
        outNx = tri.nx;
        outNy = tri.ny;
        outNz = tri.nz;
        return radius;
    }

    if (distance == 0) {
        outNx = tri.nx;
        outNy = tri.ny;
        outNz = tri.nz;
        return radius;
    }

    // Normalize push direction using 32-bit division only
    outNx = fpdiv(nx, distance);
    outNy = fpdiv(ny, distance);
    outNz = fpdiv(nz, distance);

    return radius - distance; // Penetration depth
}

// ============================================================================
// Ray vs Triangle (Möller-Trumbore, fixed-point)
// ============================================================================

int32_t WorldCollision::rayVsTriangle(int32_t ox, int32_t oy, int32_t oz,
                                       int32_t dx, int32_t dy, int32_t dz,
                                       const CollisionTri& tri) const {
    // h = cross(D, e2)
    int32_t hx, hy, hz;
    cross3(dx, dy, dz, tri.e2x, tri.e2y, tri.e2z, hx, hy, hz);

    // a = dot(e1, h)
    int32_t a = dot3(tri.e1x, tri.e1y, tri.e1z, hx, hy, hz);
    if (a > -COLLISION_EPSILON && a < COLLISION_EPSILON)
        return -1; // Ray parallel to triangle

    // f = 1/a — we'll defer the division by working with a as denominator
    // s = O - v0
    int32_t sx = ox - tri.v0x;
    int32_t sy = oy - tri.v0y;
    int32_t sz = oz - tri.v0z;

    // u = f * dot(s, h) = dot(s, h) / a
    int32_t sh = dot3(sx, sy, sz, hx, hy, hz);
    // Check u in [0, 1]: sh/a must be in [0, a] if a > 0, or [a, 0] if a < 0
    if (a > 0) {
        if (sh < 0 || sh > a) return -1;
    } else {
        if (sh > 0 || sh < a) return -1;
    }

    // q = cross(s, e1)
    int32_t qx, qy, qz;
    cross3(sx, sy, sz, tri.e1x, tri.e1y, tri.e1z, qx, qy, qz);

    // v = f * dot(D, q) = dot(D, q) / a
    int32_t dq = dot3(dx, dy, dz, qx, qy, qz);
    if (a > 0) {
        if (dq < 0 || sh + dq > a) return -1;
    } else {
        if (dq > 0 || sh + dq < a) return -1;
    }

    // t = f * dot(e2, q) = dot(e2, q) / a
    int32_t eq = dot3(tri.e2x, tri.e2y, tri.e2z, qx, qy, qz);

    // t in 20.12 using 32-bit division only
    int32_t t = fpdiv(eq, a);

    if (t < COLLISION_EPSILON) return -1; // Behind ray origin

    return t;
}

// ============================================================================
// High-level: moveAndSlide
// ============================================================================

psyqo::Vec3 WorldCollision::moveAndSlide(const psyqo::Vec3& oldPos,
                                          const psyqo::Vec3& newPos,
                                          int32_t radius,
                                          uint8_t currentRoom) const {
    if (!isLoaded()) return newPos;

    int32_t posX = newPos.x.raw();
    int32_t posY = newPos.y.raw();
    int32_t posZ = newPos.z.raw();

    // Gather candidate meshes
    uint16_t meshIndices[32];
    int meshCount = gatherCandidateMeshes(posX, posZ, currentRoom, meshIndices, 32);

    // Sphere AABB for broad phase
    int32_t sMinX, sMinY, sMinZ, sMaxX, sMaxY, sMaxZ;
    sphereToAABB(posX, posY, posZ, radius + COLLISION_EPSILON,
                 sMinX, sMinY, sMinZ, sMaxX, sMaxY, sMaxZ);

    int triTests = 0;
    int totalCollisions = 0;



    for (int iter = 0; iter < MAX_COLLISION_ITERATIONS; iter++) {
        bool collided = false;

        for (int mi = 0; mi < meshCount && triTests < MAX_TRI_TESTS_PER_FRAME; mi++) {
            const auto& mesh = m_meshes[meshIndices[mi]];

            // Broad phase: sphere AABB vs mesh AABB
            if (!aabbOverlap(sMinX, sMinY, sMinZ, sMaxX, sMaxY, sMaxZ,
                             mesh.aabbMinX, mesh.aabbMinY, mesh.aabbMinZ,
                             mesh.aabbMaxX, mesh.aabbMaxY, mesh.aabbMaxZ)) {

                continue;
            }

            for (int ti = 0; ti < mesh.triangleCount && triTests < MAX_TRI_TESTS_PER_FRAME; ti++) {
                const auto& tri = m_triangles[mesh.firstTriangle + ti];
                triTests++;

                // Skip floor and ceiling triangles — Y is resolved by nav regions.
                // In PS1 space (Y-down): floor normals have ny < 0, ceiling ny > 0.
                // If |ny| > walkable slope threshold, it's a floor/ceiling, not a wall.
                int32_t absNy = tri.ny >= 0 ? tri.ny : -tri.ny;
                if (absNy > WALKABLE_SLOPE_COS) continue;

                int32_t nx, ny, nz;
                int32_t pen = sphereVsTriangle(posX, posY, posZ, radius, tri, nx, ny, nz);

                if (pen > 0) {
                    totalCollisions++;
                    // Push out along normal
                    posX += fpmul(pen + COLLISION_EPSILON, nx);
                    posY += fpmul(pen + COLLISION_EPSILON, ny);
                    posZ += fpmul(pen + COLLISION_EPSILON, nz);

                    // Update sphere AABB
                    sphereToAABB(posX, posY, posZ, radius + COLLISION_EPSILON,
                                 sMinX, sMinY, sMinZ, sMaxX, sMaxY, sMaxZ);
                    collided = true;
                }
            }
        }

        if (!collided) break;
    }

    psyqo::Vec3 result;
    result.x.value = posX;
    result.y.value = posY;
    result.z.value = posZ;
    return result;
}

// ============================================================================
// High-level: groundTrace
// ============================================================================

bool WorldCollision::groundTrace(const psyqo::Vec3& pos,
                                  int32_t maxDist,
                                  int32_t& groundY,
                                  int32_t& groundNormalY,
                                  uint8_t& surfaceFlags,
                                  uint8_t currentRoom) const {
    if (!isLoaded()) return false;

    int32_t ox = pos.x.raw();
    int32_t oy = pos.y.raw();
    int32_t oz = pos.z.raw();

    // Ray direction: straight down (positive Y in PS1 space = down)
    int32_t dx = 0, dy = FP_ONE, dz = 0;

    uint16_t meshIndices[32];
    int meshCount = gatherCandidateMeshes(ox, oz, currentRoom, meshIndices, 32);

    int32_t bestDist = maxDist;
    bool hit = false;

    for (int mi = 0; mi < meshCount; mi++) {
        const auto& mesh = m_meshes[meshIndices[mi]];

        // Quick reject: check if mesh is below us
        if (mesh.aabbMinY > oy + maxDist) continue;
        if (mesh.aabbMaxY < oy) continue;
        if (ox < mesh.aabbMinX || ox > mesh.aabbMaxX) continue;
        if (oz < mesh.aabbMinZ || oz > mesh.aabbMaxZ) continue;

        for (int ti = 0; ti < mesh.triangleCount; ti++) {
            const auto& tri = m_triangles[mesh.firstTriangle + ti];

            int32_t t = rayVsTriangle(ox, oy, oz, dx, dy, dz, tri);
            if (t >= 0 && t < bestDist) {
                bestDist = t;
                groundY = oy + t; // Hit point Y
                groundNormalY = tri.ny;
                surfaceFlags = tri.flags;
                hit = true;
            }
        }
    }

    return hit;
}

// ============================================================================
// High-level: ceilingTrace
// ============================================================================

bool WorldCollision::ceilingTrace(const psyqo::Vec3& pos,
                                   int32_t playerHeight,
                                   int32_t& ceilingY,
                                   uint8_t currentRoom) const {
    if (!isLoaded()) return false;

    int32_t ox = pos.x.raw();
    int32_t oy = pos.y.raw();
    int32_t oz = pos.z.raw();

    // Ray direction: straight up (negative Y in PS1 space)
    int32_t dx = 0, dy = -FP_ONE, dz = 0;

    uint16_t meshIndices[32];
    int meshCount = gatherCandidateMeshes(ox, oz, currentRoom, meshIndices, 32);

    int32_t bestDist = playerHeight;
    bool hit = false;

    for (int mi = 0; mi < meshCount; mi++) {
        const auto& mesh = m_meshes[meshIndices[mi]];

        if (mesh.aabbMaxY > oy) continue;
        if (mesh.aabbMinY < oy - playerHeight) continue;
        if (ox < mesh.aabbMinX || ox > mesh.aabbMaxX) continue;
        if (oz < mesh.aabbMinZ || oz > mesh.aabbMaxZ) continue;

        for (int ti = 0; ti < mesh.triangleCount; ti++) {
            const auto& tri = m_triangles[mesh.firstTriangle + ti];

            int32_t t = rayVsTriangle(ox, oy, oz, dx, dy, dz, tri);
            if (t >= 0 && t < bestDist) {
                bestDist = t;
                ceilingY = oy - t;
                hit = true;
            }
        }
    }

    return hit;
}

// ============================================================================
// High-level: raycast (general purpose)
// ============================================================================

bool WorldCollision::raycast(int32_t ox, int32_t oy, int32_t oz,
                              int32_t dx, int32_t dy, int32_t dz,
                              int32_t maxDist,
                              CollisionHit& hit,
                              uint8_t currentRoom) const {
    if (!isLoaded()) return false;

    uint16_t meshIndices[32];
    int meshCount = gatherCandidateMeshes(ox, oz, currentRoom, meshIndices, 32);

    int32_t bestDist = maxDist;
    bool found = false;

    for (int mi = 0; mi < meshCount; mi++) {
        const auto& mesh = m_meshes[meshIndices[mi]];

        for (uint16_t ti = 0; ti < mesh.triangleCount; ti++) {
            uint16_t triIdx = mesh.firstTriangle + ti;
            const auto& tri = m_triangles[triIdx];

            int32_t t = rayVsTriangle(ox, oy, oz, dx, dy, dz, tri);
            if (t >= 0 && t < bestDist) {
                bestDist = t;
                hit.pointX = ox + fpmul(t, dx);
                hit.pointY = oy + fpmul(t, dy);
                hit.pointZ = oz + fpmul(t, dz);
                hit.normalX = tri.nx;
                hit.normalY = tri.ny;
                hit.normalZ = tri.nz;
                hit.distance = t;
                hit.triangleIndex = triIdx;
                hit.surfaceFlags = tri.flags;
                found = true;
            }
        }
    }

    return found;
}

}  // namespace psxsplash
