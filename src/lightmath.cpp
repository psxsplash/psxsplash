#include "lightmath.hh"

namespace psxsplash {
namespace lightmath {

namespace {

// Within this many 1/256ths of t = d^2/r^2 of the light (d < r/4), term() takes
// an exact path with a square root and two divides: 1/sqrt(t) is too steep there
// for a table. That is 1/64 of the lit sphere's volume.
constexpr uint32_t kExactBins = 16;

consteval double ceSqrt(double x) {
    if (x <= 0.0) return 0.0;
    double g = x > 1.0 ? x : 1.0;
    for (int i = 0; i < 64; i++) g = 0.5 * (g + x / g);
    return g;
}

// k(t) = (1 - sqrt(t)) / sqrt(t) at t = i/256, as 6.10, interpolated between
// entries. term() multiplies it by (N.d) / r, which is cos * sqrt(t), so the
// product is cos * (1 - d/r): linear falloff and Lambert with no square root and
// no divide per vertex.
struct FalloffLut {
    uint16_t k[257];
};

consteval FalloffLut makeFalloffLut() {
    FalloffLut lut{};
    for (int i = kExactBins; i <= 256; i++) {
        double s = ceSqrt(i / 256.0);
        lut.k[i] = (uint16_t)((1.0 - s) / s * 1024.0 + 0.5);
    }
    return lut;
}

constexpr FalloffLut kFalloff = makeFalloffLut();

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

// floor(2^p / d) for d < 2^24, by long division in 32-bit steps: there is no
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

// (a * b) >> s for s in [16, 48], without a variable 64-bit shift helper.
uint32_t mulShr(uint32_t a, uint32_t b, int s) {
    uint64_t p = (uint64_t)a * b;
    uint32_t hi = (uint32_t)(p >> 32), lo = (uint32_t)p;
    if (s >= 32) return hi >> (s - 32);
    return (hi << (32 - s)) | (lo >> s);
}

inline int32_t iabs(int32_t v) { return v < 0 ? -v : v; }

}  // namespace

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
        // p = 31 + floor(log2(radiusSq)) puts the quotient in (2^30, 2^31].
        int log2 = 0;  // MIPS I has no clz, and libgcc's is not linked
        while (((uint32_t)o.radiusSq >> log2) > 1) log2++;
        o.invRadiusSq = pow2Div(31 + log2, (uint32_t)o.radiusSq);
        // distSq * inv >> invShift = 65536 * distSq / radiusSq.
        o.invShift = (uint8_t)(31 + log2 - 16);
        o.invRadius = (1 << 24) / r;
        o.cr = (kept[i]->r * kept[i]->intensity) >> 12;
        o.cg = (kept[i]->g * kept[i]->intensity) >> 12;
        o.cb = (kept[i]->b * kept[i]->intensity) >> 12;
    }
    out.count = n;
    return n;
}

int32_t term(const ObjectLight& light, int32_t lDotV, int32_t vLenSq, int32_t nDotL,
             int32_t nDotV) {
    int32_t distSq = light.lenSq - 2 * lDotV + vLenSq;
    if (distSq >= light.radiusSq) return 0;
    int32_t nd = nDotL - nDotV;
    if (nd < 0) return 0;
    if (distSq <= 0) return 4096;

    // t = distSq / radiusSq as 0.16.
    uint32_t t16 = mulShr((uint32_t)distSq, light.invRadiusSq, light.invShift);
    uint32_t bin = t16 >> 8;
    if (bin >= 256) return 0;

    if (bin < kExactBins) {
        // distSq < radiusSq / 16 <= 2^22 here, so it takes 8 more bits: dist is
        // the distance with 4 fractional bits.
        int32_t dist = (int32_t)isqrt((uint32_t)distSq << 8);
        if (dist == 0) return 4096;
        int32_t cosine = (nd << 4) / dist;  // nd is 4096 * |d| * cos
        if (cosine > 4096) cosine = 4096;
        int32_t r16 = light.radius << 4;
        int32_t t = cosine * (r16 - dist) / r16;
        return t < 0 ? 0 : t;
    }

    uint32_t f = t16 & 0xFF;
    int32_t k = (int32_t)((kFalloff.k[bin] * (256 - f) + kFalloff.k[bin + 1] * f) >> 8);
    int32_t q = (int32_t)(((int64_t)nd * light.invRadius) >> 24);  // cos * d / r, 4.12
    int32_t t = (q * k) >> 10;
    return t > 4096 ? 4096 : t;
}

}  // namespace lightmath
}  // namespace psxsplash
