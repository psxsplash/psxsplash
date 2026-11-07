#include "backgroundmath.hh"

namespace psxsplash {
namespace backgroundmath {

uint16_t count(const uint8_t* pack, uint32_t tableOffset) {
    if (!pack || tableOffset == 0) return 0;
    return reinterpret_cast<const BackgroundTableHeader*>(pack + tableOffset)->count;
}

const BackgroundRecord* record(const uint8_t* pack, uint32_t tableOffset, int i) {
    if (i < 0 || i >= (int)count(pack, tableOffset)) return nullptr;
    return reinterpret_cast<const BackgroundRecord*>(pack + tableOffset + sizeof(BackgroundTableHeader)) + i;
}

static bool sameName(const char* a, const char* b) {
    while (*a && *a == *b) a++, b++;
    return *a == *b;
}

int find(const uint8_t* pack, uint32_t tableOffset, const char* name) {
    if (!name) return BACKGROUND_NONE;
    const int n = count(pack, tableOffset);
    for (int i = 0; i < n; i++) {
        const BackgroundRecord* r = record(pack, tableOffset, i);
        if (r->nameOffset && sameName(reinterpret_cast<const char*>(pack + r->nameOffset), name)) return i;
    }
    return BACKGROUND_NONE;
}

int startIndex(const uint8_t* pack, uint32_t tableOffset) {
    const int n = count(pack, tableOffset);
    for (int i = 0; i < n; i++)
        if (record(pack, tableOffset, i)->flags & BACKGROUND_FLAG_SHOW_AT_START) return i;
    return BACKGROUND_NONE;
}

int32_t imageBucket(uint32_t otSize) { return otSize >= 2 ? (int32_t)otSize - 2 : 0; }

int32_t layerBucket(uint16_t nearSZ, uint32_t otSize, int32_t worldDepthMin) {
    int32_t b = nearSZ;
    const int32_t hi = imageBucket(otSize);
    if (b > hi) b = hi;
    if (b < worldDepthMin) b = worldDepthMin;
    return b;
}

}  // namespace backgroundmath
}  // namespace psxsplash
