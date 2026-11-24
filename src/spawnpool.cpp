#include "spawnpool.hh"

#include "streq.hh"

namespace psxsplash {

namespace {
uint16_t readU16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }
uint32_t readU32(const uint8_t* p) { return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24); }
}  // namespace

const char* spawnpool::validateTable(const uint8_t* table, uint16_t gameObjectCount) {
    if (table == nullptr) return "Spawn pool table is missing";
    const uint16_t poolCount = readU16(table);
    if (poolCount > MAX_SPAWN_POOLS) return "Spawn pool table has too many pools";
    const uint8_t* rec = table + 4;
    for (uint16_t i = 0; i < poolCount; i++) {
        const uint16_t first = readU16(rec + i * 8);
        const uint16_t count = readU16(rec + i * 8 + 2);
        if (count == 0) return "Spawn pool is empty";
        if (uint32_t(first) + count > gameObjectCount) return "Spawn pool runs past the last object";
        if (readU32(rec + i * 8 + 4) == 0) return "Spawn pool has no name";
        for (uint16_t j = 0; j < i; j++) {
            const uint16_t f2 = readU16(rec + j * 8);
            const uint16_t c2 = readU16(rec + j * 8 + 2);
            if (first < uint32_t(f2) + c2 && f2 < uint32_t(first) + count) return "Spawn pools overlap";
        }
    }
    return nullptr;
}

void SpawnPools::load(const uint8_t* data, const uint8_t* table) {
    unload();
    if (table == nullptr) return;
    const int n = readU16(table);
    if (n == 0) return;
    m_pools = new SpawnPool[n];
    m_count = n;
    for (int i = 0; i < n; i++) {
        const uint8_t* rec = table + 4 + i * 8;
        m_pools[i].first = readU16(rec);
        m_pools[i].count = readU16(rec + 2);
        m_pools[i].name = reinterpret_cast<const char*>(data + readU32(rec + 4));
    }
}

void SpawnPools::unload() {
    delete[] m_pools;
    m_pools = nullptr;
    m_count = 0;
}

int SpawnPools::find(const char* name) const {
    if (name == nullptr) return NONE;
    for (int i = 0; i < m_count; i++) {
        if (streq(m_pools[i].name, name)) return i;
    }
    return NONE;
}

}  // namespace psxsplash
