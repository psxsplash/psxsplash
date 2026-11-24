#pragma once

#include <stdint.h>

// Spawn pools: runs of identical, inactive copies of an object that Lua hands
// out (Scene.Spawn) and takes back (Scene.Despawn). Nothing is allocated when a
// copy is spawned: the exporter writes every copy into the object table, and a
// pool only decides which one is free. Free of psyqo, so that
// tests/host/test_spawnpool.cpp compiles this exact code natively. The Lua
// side lives in luaapi.cpp and the "is it active" question is answered by the
// caller, so this file never sees a GameObject.
//
// The table (v33, header word spawnPoolTableOffset, 0 = none; 4-byte aligned):
//
//   uint16 poolCount, uint16 reserved
//   poolCount records of 8 bytes: uint16 firstObjectIndex, uint16 count,
//                                 uint32 nameOffset
//
// nameOffset is a byte offset from the start of the splashpack to a
// NUL-terminated name, the same convention as the sprite, audio, cutscene and
// animation names. The copies are the objects firstObjectIndex up to
// firstObjectIndex + count - 1, written contiguously in the object table.

namespace psxsplash {

struct SpawnPoolRecord {
    uint16_t first;
    uint16_t count;
    uint32_t nameOffset;
};
static_assert(sizeof(SpawnPoolRecord) == 8, "SpawnPoolRecord must be 8 bytes");

/// Pools one scene can hold. The exporter refuses more.
static constexpr int MAX_SPAWN_POOLS = 64;

namespace spawnpool {

/// Checks a table against a scene's object count: every run inside the object
/// table, no empty run, no two runs sharing an object, a name for each. Returns
/// nullptr when the table is sound and otherwise a message that says what is
/// wrong, which the loader hands to psyqo::Kernel::assert.
const char* validateTable(const uint8_t* table, uint16_t gameObjectCount);

}  // namespace spawnpool

class SpawnPools {
  public:
    static constexpr int NONE = -1;

    /// Reads a table validateTable accepted. The pool array is allocated here,
    /// once, from the pack's pool count; `data` is the start of the splashpack
    /// that the names are offsets into. A null table leaves no pools.
    void load(const uint8_t* data, const uint8_t* table);
    void unload();

    int poolCount() const { return m_count; }
    uint16_t first(int pool) const { return m_pools[pool].first; }
    uint16_t capacity(int pool) const { return m_pools[pool].count; }
    const char* name(int pool) const { return m_pools[pool].name; }

    /// The pool called `name`, or NONE. The first of two equal names wins.
    int find(const char* name) const;

    /// The pool that owns object `objectIndex`, or NONE.
    int poolOf(uint32_t objectIndex) const {
        for (int i = 0; i < m_count; i++) {
            if (objectIndex >= m_pools[i].first && objectIndex - m_pools[i].first < m_pools[i].count) return i;
        }
        return NONE;
    }

    /// The lowest-indexed object of `pool` for which isActive(index) is false,
    /// or -1 when every copy is out.
    template <typename IsActive>
    int pickFree(int pool, IsActive&& isActive) const {
        const SpawnPool& p = m_pools[pool];
        for (uint32_t i = 0; i < p.count; i++) {
            if (!isActive(uint32_t(p.first) + i)) return int(p.first + i);
        }
        return -1;
    }

    /// How many copies of `pool` are active.
    template <typename IsActive>
    uint16_t activeCount(int pool, IsActive&& isActive) const {
        const SpawnPool& p = m_pools[pool];
        uint16_t n = 0;
        for (uint32_t i = 0; i < p.count; i++) {
            if (isActive(uint32_t(p.first) + i)) n++;
        }
        return n;
    }

  private:
    struct SpawnPool {
        uint16_t first;
        uint16_t count;
        const char* name;
    };
    SpawnPool* m_pools = nullptr;
    int m_count = 0;
};

}  // namespace psxsplash
