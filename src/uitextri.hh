#pragma once

#include <stdint.h>

#include "uiblend.hh"

// The arithmetic behind Lua's UI.DrawTexturedTriangle, free of psyqo so
// tests/host/test_uitextri.cpp compiles this exact code natively.
//
// The texture is a scene sprite sheet. Lua gives texture coordinates in
// texels from the sheet texture's top-left corner; the GPU wants them from the
// texture page's top-left, as one byte each.

namespace psxsplash {
namespace uitextri {

/// A Lua texture coordinate (texels from the sheet's corner) as the byte the
/// GPU takes (texels from the page's corner), clamped to the page.
///
/// The exporter refuses a sheet whose texture runs past its 256x256 page
/// (splashedit spriteSheetPageError: base + size <= 256), so every texel of
/// the sheet's own texture fits and is passed through exactly. A coordinate
/// past the page is the script reading off the texture; wrapping the byte
/// would sample the far side of the page (another texture), so it clamps to
/// the page's last (or first) texel instead.
constexpr uint8_t texCoord(uint8_t base, int32_t rel) {
    int32_t a = (int32_t)base + rel;
    if (a < 0) a = 0;
    if (a > 255) a = 255;
    return (uint8_t)a;
}

/// A blend given by name, as UI.DrawTexturedTriangle takes it. "opaque" and
/// the four modes in GPU order; anything else (a typo, nil) reads as opaque.
inline UIBlend blendFromName(const char* s) {
    if (!s) return UIBlend::Opaque;
    struct Entry {
        const char* name;
        UIBlend blend;
    };
    static constexpr Entry kNames[] = {
        {"half", UIBlend::Half},
        {"add", UIBlend::Add},
        {"subtract", UIBlend::Subtract},
        {"addQuarter", UIBlend::AddQuarter},
    };
    for (const Entry& e : kNames) {
        const char* a = e.name;
        const char* b = s;
        while (*a && *a == *b) a++, b++;
        if (*a == 0 && *b == 0) return e.blend;
    }
    return UIBlend::Opaque;
}

/// A blend given by number: the UIBlend codes, 1-4 the modes, 0 and anything
/// else opaque.
constexpr UIBlend blendFromCode(int32_t code) {
    return code >= 1 && code <= 4 ? static_cast<UIBlend>(code) : UIBlend::Opaque;
}

/// FNV-1a of a sheet name: the key a missing sheet is reported under.
constexpr uint32_t nameKey(const char* s) {
    uint32_t h = 2166136261u;
    while (s && *s) h = (h ^ (uint8_t)*s++) * 16777619u;
    return h;
}

/// The key a bad sheet index is reported under. The top bit keeps it apart
/// from most name keys; a clash only costs one missed log line.
constexpr uint32_t indexKey(int32_t index) { return 0x80000000u | ((uint32_t)index & 0x7fffffffu); }

/// The keys already logged, so a script drawing a missing sheet every frame
/// prints its name once. Fixed size, nothing allocated; once full, further
/// new names are not logged (they still draw the placeholder).
struct ReportedNames {
    static constexpr int kMax = 16;
    uint32_t keys[kMax] = {};
    int count = 0;

    /// True the first time `key` is seen, while there is room to remember it.
    bool first(uint32_t key) {
        for (int i = 0; i < count; i++)
            if (keys[i] == key) return false;
        if (count >= kMax) return false;
        keys[count++] = key;
        return true;
    }
    void clear() { count = 0; }
};

}  // namespace uitextri
}  // namespace psxsplash
