#include "lightmath.hh"

namespace psxsplash {
namespace lightmath {

namespace {

consteval double ceSqrt(double x) {
    if (x <= 0.0) return 0.0;
    double g = x > 1.0 ? x : 1.0;
    for (int i = 0; i < 64; i++) g = 0.5 * (g + x / g);
    return g;
}

// Filled from kExactBins up. Below it shade() calls termExact(); that is d < r/4,
// 1/64 of the lit sphere's volume.
consteval FalloffLut makeFalloffLut() {
    FalloffLut lut{};
    for (uint32_t i = kExactBins; i < kLutBins; i++) {
        double s = ceSqrt((i + 0.5) / kLutBins);
        lut.k[i] = (uint16_t)((1.0 - s) / s * 1024.0 + 0.5);
    }
    return lut;
}

// floor(2^p / d) for d < 2^30, by long division in 32-bit steps: there is no
// 64-bit divide on this target. The caller picks p so the quotient fits.
uint32_t pow2Div(int p, uint32_t d) {
    uint32_t rem = 1, q = rem >= d ? 1 : 0;
    if (q) rem -= d;
    for (int i = 0; i < p; i++) {
        rem <<= 1;
        q <<= 1;
        if (rem >= d) {
            rem -= d;
            q |= 1;
        }
    }
    return q;
}

uint32_t isqrt(uint32_t n) {
    uint32_t root = 0, bit = 1u << 30;
    while (bit > n) bit >>= 2;
    while (bit) {
        if (n >= root + bit) {
            n -= root + bit;
            root = (root >> 1) + bit;
        } else {
            root >>= 1;
        }
        bit >>= 2;
    }
    return root;
}

inline int32_t iabs(int32_t v) { return v < 0 ? -v : v; }

}  // namespace

constinit const FalloffLut kFalloffLut = makeFalloffLut();

bool sphereTouchesAABB(int32_t cx, int32_t cy, int32_t cz, int32_t radius,
                       const int32_t boxMin[3], const int32_t boxMax[3]) {
    if (radius <= 0) return false;
    const int32_t c[3] = {cx, cy, cz};
    int64_t distSq = 0;
    for (int i = 0; i < 3; i++) {
        int32_t d = 0;
        if (c[i] < boxMin[i]) d = boxMin[i] - c[i];
        else if (c[i] > boxMax[i]) d = c[i] - boxMax[i];
        if (d >= radius) return false;
        distSq += (int64_t)d * d;
    }
    return distSq < (int64_t)radius * radius;
}

int prepare(const PointLight* lights, int lightCount, const int32_t position[3],
            const int32_t rot[3][3], const int32_t aabbMin[3], const int32_t aabbMax[3],
            ObjectLights& out) {
    out.count = 0;
    out.shift = kMinShift;
    if (!lights || lightCount <= 0) return 0;

    int32_t local[MAX_LIGHTS_PER_MESH][3];
    const PointLight* kept[MAX_LIGHTS_PER_MESH];
    int n = 0;
    int32_t largest = 0;

    for (int i = 0; i < lightCount && n < MAX_LIGHTS_PER_MESH; i++) {
        const PointLight& l = lights[i];
        if (!l.enabled) continue;
        if (!sphereTouchesAABB(l.x, l.y, l.z, l.radius, aabbMin, aabbMax)) continue;

        const int32_t rel[3] = {l.x - position[0], l.y - position[1], l.z - position[2]};
        for (int j = 0; j < 3; j++) {
            int64_t acc = (int64_t)rot[0][j] * rel[0] + (int64_t)rot[1][j] * rel[1] +
                          (int64_t)rot[2][j] * rel[2];
            int32_t v = (int32_t)(acc >> 12);
            local[n][j] = v;
            if (iabs(v) > largest) largest = iabs(v);
        }
        if (l.radius > largest) largest = l.radius;
        kept[n++] = &l;
    }
    if (n == 0) return 0;

    int shift = kMinShift;
    while ((largest >> shift) > kOperandMax) shift++;
    out.shift = shift;
    out.flatScale = shift > 12 ? 0 : -(4096 >> shift);

    for (int i = 0; i < n; i++) {
        ObjectLight& o = out.lights[i];
        o.lx = shiftRound(local[i][0], shift);
        o.ly = shiftRound(local[i][1], shift);
        o.lz = shiftRound(local[i][2], shift);
        o.lenSq = o.lx * o.lx + o.ly * o.ly + o.lz * o.lz;
        int32_t r = kept[i]->radius >> shift;
        if (r < 1) r = 1;
        o.radius = r;
        o.radiusSq = r * r;
        int tShift = 0;
        while ((((uint32_t)o.radiusSq << tShift) & 0x80000000u) == 0) tShift++;
        o.tShift = (uint8_t)tShift;
        // 2^52 / (R << tShift) = 2^44 / ((R << tShift) >> 8), divisor in [2^23, 2^24).
        o.invRadiusSq = pow2Div(44, ((uint32_t)o.radiusSq << tShift) >> 8);
        o.invRadius = 0xFFFFFFFFu / (uint32_t)r;
        o.cr = (kept[i]->r * kept[i]->intensity) >> 12;
        o.cg = (kept[i]->g * kept[i]->intensity) >> 12;
        o.cb = (kept[i]->b * kept[i]->intensity) >> 12;
        o.sceneIndex = (uint8_t)(kept[i] - lights);
        o.colourLut = nullptr;
    }
    out.count = n;
    return n;
}

uint32_t reachMask(const ObjectLights& ol, const int32_t v[3][3]) {
    int32_t lo[3], hi[3];
    for (int a = 0; a < 3; a++) {
        lo[a] = hi[a] = v[0][a];
        for (int k = 1; k < 3; k++) {
            if (v[k][a] < lo[a]) lo[a] = v[k][a];
            if (v[k][a] > hi[a]) hi[a] = v[k][a];
        }
    }
    uint32_t mask = 0;
    for (int i = 0; i < ol.count; i++) {
        const ObjectLight& l = ol.lights[i];
        const int32_t c[3] = {l.lx, l.ly, l.lz};
        bool reach = true;
        for (int a = 0; a < 3 && reach; a++) {
            if (c[a] <= lo[a] - l.radius || c[a] >= hi[a] + l.radius) reach = false;
        }
        if (reach) mask |= 1u << i;
    }
    return mask;
}

int32_t termExact(const ObjectLight& light, int32_t distSq, int32_t nd) {
    if (distSq <= 0) return 4096;
    // distSq < radiusSq / 16 <= 2^22 here, so it takes 8 more bits: dist is the
    // distance with 4 fractional bits.
    int32_t dist = (int32_t)isqrt((uint32_t)distSq << 8);
    if (dist == 0) return 4096;
    int32_t cosine = (nd << 4) / dist;  // nd is 4096 * |d| * cos
    if (cosine > 4096) cosine = 4096;
    int32_t r16 = light.radius << 4;
    int32_t t = cosine * (r16 - dist) / r16;
    return t < 0 ? 0 : t;
}

}  // namespace lightmath
}  // namespace psxsplash
