#pragma once

#include <stdint.h>

// Shine (environment-mapped reflections) arithmetic, free of psyqo and the GTE
// so that tests/host/test_shine.cpp compiles this exact code natively.
//
// A shiny triangle (Tri flag SHINE_FLAG) is drawn twice: its own material,
// then the same triangle again through the object's reflection texture,
// semi-transparent (additive, or the GPU's 50/50 average). The reflection's
// UVs are computed every frame from the triangle's three vertex normals
// turned into view space by the GTE (RT, no translation):
//
//   u = u0 + w/2 + nx * (w/2),  v = v0 + h/2 + ny * (h/2)   (n in 4.12)
//
// clamped into the w x h rectangle. View y points down the screen, so a
// normal facing up (ny < 0) reads the top of the texture: a sky at the top of
// the reflection texture shows on the upper side of the object, wherever the
// camera looks from. The overlay is grey `strength` (128 = the texture as is),
// faded with the fog like the base texture is, so the fog pass drawn after it
// is not undone.
//
// Table (splashpack v30, header shineTableOffset): u16 count, u16 0, then per
// shiny object a ShineRecord followed by triCount x 3 vertex normals (int16
// x, y, z, 4.12), for every triangle of the object in polygon order (the ones
// without SHINE_FLAG too, so the triangle index reaches them directly).

namespace psxsplash {

/// Shiny objects a scene can hold; the loader drops the rest.
static constexpr int MAX_SCENE_SHINES = 64;

enum ShineBlend : uint8_t {
    SHINE_ADD = 0,   ///< background + overlay
    SHINE_HALF = 1,  ///< (background + overlay) / 2
};

struct ShineRecord {
    uint16_t gameObjectIndex;
    uint16_t triCount;  ///< the object's polyCount: normals that follow
    uint16_t tpage;     ///< TPageAttr of the reflection texture (its blend bits are replaced)
    uint16_t clutX;     ///< as Tri::clutX / clutY
    uint16_t clutY;
    uint8_t u0, v0;     ///< the reflection texture's rectangle in its texture page
    uint8_t wMinus1, hMinus1;
    uint8_t strength;   ///< overlay grey, 0..128
    uint8_t blend;      ///< ShineBlend
};
static_assert(sizeof(ShineRecord) == 16, "ShineRecord must be 16 bytes");

namespace shinemath {

/// Normals of triangle t of record r: 3 x (x, y, z).
inline const int16_t* normals(const ShineRecord* r, uint16_t t) {
    return reinterpret_cast<const int16_t*>(r + 1) + t * 9;
}

/// Walks a shine table (`table` points at its count) and sets byObject[i] to
/// the record of object i, for records whose object index is below
/// objectCount and whose triCount matches polyCount(index); returns how many
/// were kept (at most MAX_SCENE_SHINES). byObject must hold objectCount
/// entries, cleared by the caller.
template <typename PolyCount>
inline int parseTable(const uint8_t* table, uint16_t objectCount, PolyCount polyCount, const ShineRecord** byObject) {
    const uint16_t count = uint16_t(table[0] | (table[1] << 8));
    const uint8_t* p = table + 4;
    int kept = 0;
    for (uint16_t i = 0; i < count; i++) {
        const ShineRecord* r = reinterpret_cast<const ShineRecord*>(p);
        p += sizeof(ShineRecord) + uint32_t(r->triCount) * 18;
        if (r->gameObjectIndex >= objectCount || r->triCount != polyCount(r->gameObjectIndex)) continue;
        if (r->blend > SHINE_HALF || kept >= MAX_SCENE_SHINES) continue;
        byObject[r->gameObjectIndex] = r;
        kept++;
    }
    return kept;
}

/// One reflection texture coordinate from a view-space normal component n
/// (4.12): base + size/2 + n * size/2, inside [base, base + sizeMinus1].
inline uint8_t envCoord(int32_t n, uint8_t base, uint8_t sizeMinus1) {
    const int32_t half = (int32_t(sizeMinus1) + 1) >> 1;
    int32_t c = half + ((n * half) >> 12);
    if (c < 0) c = 0;
    if (c > sizeMinus1) c = sizeMinus1;
    return uint8_t(base + c);
}

/// The overlay's grey at a vertex with fog amount ir (4.12, as the
/// renderer's fogIR): strength * (1 - fog), so the overlay fades into the
/// fog exactly as the base texture does.
inline uint8_t overlayGrey(uint8_t strength, int32_t ir) {
    if (ir <= 0) return strength;
    if (ir >= 4096) return 0;
    return uint8_t((strength * (4096 - ir)) >> 12);
}

}  // namespace shinemath
}  // namespace psxsplash
