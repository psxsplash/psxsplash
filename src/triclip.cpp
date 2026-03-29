#include "triclip.hh"

namespace psxsplash {

// ============================================================================
// Screen-space Sutherland-Hodgman clipping
// ============================================================================

// Interpolate between two ClipVertex at parameter t (0..256 = 0.0..1.0, fp8).
// Uses fp8 to avoid overflow with int16 coordinates (int16 * 256 fits int32).
static ClipVertex lerpClip(const ClipVertex& a, const ClipVertex& b, int32_t t) {
    ClipVertex r;
    r.x = (int16_t)(a.x + (((int32_t)(b.x - a.x) * t) >> 8));
    r.y = (int16_t)(a.y + (((int32_t)(b.y - a.y) * t) >> 8));
    r.z = (int16_t)(a.z + (((int32_t)(b.z - a.z) * t) >> 8));
    r.u = (uint8_t)(a.u + (((int)(b.u) - (int)(a.u)) * t >> 8));
    r.v = (uint8_t)(a.v + (((int)(b.v) - (int)(a.v)) * t >> 8));
    r.r = (uint8_t)(a.r + (((int)(b.r) - (int)(a.r)) * t >> 8));
    r.g = (uint8_t)(a.g + (((int)(b.g) - (int)(a.g)) * t >> 8));
    r.b = (uint8_t)(a.b + (((int)(b.b) - (int)(a.b)) * t >> 8));
    return r;
}

// Clip a polygon (in[] with inCount vertices) against a single edge.
// Edge is defined by axis (0=X, 1=Y), sign (+1 or -1), and threshold.
// Output written to out[], returns output vertex count.
static int clipEdge(const ClipVertex* in, int inCount,
                    ClipVertex* out, int axis, int sign, int16_t threshold) {
    if (inCount == 0) return 0;
    int outCount = 0;

    for (int i = 0; i < inCount; i++) {
        const ClipVertex& cur = in[i];
        const ClipVertex& next = in[(i + 1) % inCount];

        int16_t curVal  = (axis == 0) ? cur.x  : cur.y;
        int16_t nextVal = (axis == 0) ? next.x : next.y;

        bool curInside  = (sign > 0) ? (curVal  <= threshold) : (curVal  >= threshold);
        bool nextInside = (sign > 0) ? (nextVal <= threshold) : (nextVal >= threshold);

        if (curInside) {
            if (outCount < 8) out[outCount++] = cur;
            if (!nextInside) {
                // Exiting: compute intersection
                int32_t den = (int32_t)nextVal - (int32_t)curVal;
                if (den != 0) {
                    int32_t t = ((int32_t)(threshold - curVal) << 8) / den;
                    if (t < 0) t = 0;
                    if (t > 256) t = 256;
                    if (outCount < 8) out[outCount++] = lerpClip(cur, next, t);
                }
            }
        } else if (nextInside) {
            // Entering: compute intersection
            int32_t den = (int32_t)nextVal - (int32_t)curVal;
            if (den != 0) {
                int32_t t = ((int32_t)(threshold - curVal) << 8) / den;
                if (t < 0) t = 0;
                if (t > 256) t = 256;
                if (outCount < 8) out[outCount++] = lerpClip(cur, next, t);
            }
        }
    }
    return outCount;
}

int clipTriangle(const ClipVertex& v0, const ClipVertex& v1, const ClipVertex& v2,
                 ClipResult& result) {
    // Working buffers for Sutherland-Hodgman (max 8 vertices after 4 clips).
    ClipVertex bufA[8], bufB[8];
    bufA[0] = v0; bufA[1] = v1; bufA[2] = v2;
    int count = 3;

    // Clip against 4 edges: left, right, top, bottom.
    count = clipEdge(bufA, count, bufB, 0, -1, CLIP_LEFT);    // X >= CLIP_LEFT
    count = clipEdge(bufB, count, bufA, 0, +1, CLIP_RIGHT);   // X <= CLIP_RIGHT
    count = clipEdge(bufA, count, bufB, 1, -1, CLIP_TOP);     // Y >= CLIP_TOP
    count = clipEdge(bufB, count, bufA, 1, +1, CLIP_BOTTOM);  // Y <= CLIP_BOTTOM

    if (count < 3) return 0;

    // Triangulate the convex polygon into a fan from vertex 0.
    int triCount = count - 2;
    if (triCount > MAX_CLIP_TRIS) triCount = MAX_CLIP_TRIS;

    for (int i = 0; i < triCount; i++) {
        result.verts[i * 3 + 0] = bufA[0];
        result.verts[i * 3 + 1] = bufA[i + 1];
        result.verts[i * 3 + 2] = bufA[i + 2];
    }
    return triCount;
}

// ============================================================================
// Near-plane (3D view-space) clipping
// ============================================================================

ViewVertex lerpViewVertex(const ViewVertex& a, const ViewVertex& b, int32_t t) {
    ViewVertex r;
    r.x = a.x + (int32_t)(((int64_t)(b.x - a.x) * t) >> 12);
    r.y = a.y + (int32_t)(((int64_t)(b.y - a.y) * t) >> 12);
    r.z = a.z + (int32_t)(((int64_t)(b.z - a.z) * t) >> 12);
    r.u = (uint8_t)(a.u + (((int)(b.u) - (int)(a.u)) * t >> 12));
    r.v = (uint8_t)(a.v + (((int)(b.v) - (int)(a.v)) * t >> 12));
    r.r = (uint8_t)(a.r + (((int)(b.r) - (int)(a.r)) * t >> 12));
    r.g = (uint8_t)(a.g + (((int)(b.g) - (int)(a.g)) * t >> 12));
    r.b = (uint8_t)(a.b + (((int)(b.b) - (int)(a.b)) * t >> 12));
    r.pad = 0;
    return r;
}

static inline int32_t nearPlaneT(int32_t zA, int32_t zB) {
    constexpr int32_t nearZ = (int32_t)NEAR_PLANE_Z << 12;
    int32_t num = nearZ - zA;
    int32_t den = zB - zA;
    if (den == 0) return 0;
    int32_t absNum = num < 0 ? -num : num;
    int shift = 0;
    while (absNum > 0x7FFFF) {
        absNum >>= 1;
        shift++;
    }
    if (shift > 0) {
        num >>= shift;
        den >>= shift;
        if (den == 0) return num > 0 ? 4096 : 0;
    }
    return (num << 12) / den;
}

static inline bool isBehind(int32_t z) {
    return z < ((int32_t)NEAR_PLANE_Z << 12);
}

int nearPlaneClip(const ViewVertex& v0, const ViewVertex& v1, const ViewVertex& v2,
                  NearClipResult& result) {
    bool b0 = isBehind(v0.z);
    bool b1 = isBehind(v1.z);
    bool b2 = isBehind(v2.z);
    int behindCount = (int)b0 + (int)b1 + (int)b2;

    if (behindCount == 3) {
        result.triCount = 0;
        return 0;
    }
    if (behindCount == 0) {
        result.triCount = 1;
        result.verts[0] = v0;
        result.verts[1] = v1;
        result.verts[2] = v2;
        return 1;
    }
    if (behindCount == 1) {
        const ViewVertex* A;
        const ViewVertex* B;
        const ViewVertex* C;
        if (b0)      { A = &v0; B = &v1; C = &v2; }
        else if (b1)  { A = &v1; B = &v2; C = &v0; }
        else          { A = &v2; B = &v0; C = &v1; }

        int32_t tAB = nearPlaneT(A->z, B->z);
        int32_t tAC = nearPlaneT(A->z, C->z);
        ViewVertex AB = lerpViewVertex(*A, *B, tAB);
        ViewVertex AC = lerpViewVertex(*A, *C, tAC);

        result.triCount = 2;
        result.verts[0] = AB;
        result.verts[1] = *B;
        result.verts[2] = *C;
        result.verts[3] = AB;
        result.verts[4] = *C;
        result.verts[5] = AC;
        return 2;
    }
    {
        const ViewVertex* A;
        const ViewVertex* B;
        const ViewVertex* C;
        if (!b0)      { A = &v0; B = &v1; C = &v2; }
        else if (!b1)  { A = &v1; B = &v2; C = &v0; }
        else          { A = &v2; B = &v0; C = &v1; }

        int32_t tBA = nearPlaneT(B->z, A->z);
        int32_t tCA = nearPlaneT(C->z, A->z);
        ViewVertex BA = lerpViewVertex(*B, *A, tBA);
        ViewVertex CA = lerpViewVertex(*C, *A, tCA);

        result.triCount = 1;
        result.verts[0] = *A;
        result.verts[1] = BA;
        result.verts[2] = CA;
        return 1;
    }
}

}  // namespace psxsplash
