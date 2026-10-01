#pragma once

#include <stdint.h>

namespace psxsplash {

// Pure policy half of WorldStreamer: which regions should be resident and where
// they live in the pool. No psyqo, no I/O, so tests/host builds it natively.

struct SPLASHPACKStreamTable {
    uint16_t regionCount;
    uint16_t objectRefCount;
    uint32_t poolBytes;     // RAM reserved for resident regions; exporter-sized
    int32_t loadRadius;     // fp12: load a region once the camera is this close
    int32_t unloadRadius;   // fp12: drop it beyond this (> loadRadius)
};
static_assert(sizeof(SPLASHPACKStreamTable) == 16, "SPLASHPACKStreamTable must be 16 bytes");

struct SPLASHPACKStreamRegion {
    int32_t minX, minZ, maxX, maxZ;  // fp12 XZ bounds of the region's geometry
    uint32_t firstSector;            // in the .GEO file
    uint32_t byteSize;               // multiple of 2048
    uint16_t firstObjectRef;
    uint16_t objectRefCount;
    uint32_t pad;
};
static_assert(sizeof(SPLASHPACKStreamRegion) == 32, "SPLASHPACKStreamRegion must be 32 bytes");

struct SPLASHPACKStreamObjectRef {
    uint16_t objectIndex;
    uint16_t polyCount;
    uint32_t offset;  // byte offset of this object's Tri array inside the region blob
};
static_assert(sizeof(SPLASHPACKStreamObjectRef) == 8, "SPLASHPACKStreamObjectRef must be 8 bytes");

/// Squared XZ distance (fp12^2) from a point to a region's bounds; 0 inside.
int64_t streamRegionDistSq(const SPLASHPACKStreamRegion& r, int32_t x, int32_t z);

class StreamPlanner {
  public:
    static constexpr int kMaxResident = 32;
    static constexpr int kNone = -1;

    void reset(const SPLASHPACKStreamTable* table, const SPLASHPACKStreamRegion* regions);

    /// A resident region beyond unloadRadius, or kNone. Call until kNone.
    int nextUnload(int32_t x, int32_t z) const;

    /**
     * The nearest non-resident, non-pending region within loadRadius, with pool
     * space for it found by evicting resident regions outside loadRadius
     * (farthest first). Evictions are reported through `evicted` (up to
     * kMaxResident entries, count in `evictedCount`) and already applied.
     * Returns kNone if nothing needs loading or no space can be made.
     */
    int planLoad(int32_t x, int32_t z, uint32_t& outOffset, uint16_t* evicted, int& evictedCount);

    void beginPending(int region, uint32_t offset);
    void finishPending(bool success);  // success makes it resident
    bool hasPending() const { return m_pendingRegion != kNone; }

    void markResident(int region, uint32_t offset);  // initial blocking loads
    void evict(int region);

    bool isResident(int region) const;
    uint32_t residentOffset(int region) const;
    int residentCount() const { return m_count; }
    int residentRegion(int slot) const { return m_slots[slot].region; }

  private:
    struct Slot {
        uint16_t region;
        uint32_t offset;
    };

    bool firstFit(uint32_t size, uint32_t& outOffset) const;

    const SPLASHPACKStreamTable* m_table = nullptr;
    const SPLASHPACKStreamRegion* m_regions = nullptr;
    Slot m_slots[kMaxResident];
    int m_count = 0;
    int m_pendingRegion = kNone;
    uint32_t m_pendingOffset = 0;
};

}  // namespace psxsplash
