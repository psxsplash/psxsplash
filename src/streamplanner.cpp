#include "streamplanner.hh"

namespace psxsplash {

int64_t streamRegionDistSq(const SPLASHPACKStreamRegion& r, int32_t x, int32_t z) {
    int32_t dx = x < r.minX ? r.minX - x : (x > r.maxX ? x - r.maxX : 0);
    int32_t dz = z < r.minZ ? r.minZ - z : (z > r.maxZ ? z - r.maxZ : 0);
    // fp12 squared; |dx| < 2^31, so the sum fits in int64.
    return static_cast<int64_t>(dx) * dx + static_cast<int64_t>(dz) * dz;
}

static int64_t sq(int32_t v) { return static_cast<int64_t>(v) * v; }

void StreamPlanner::reset(const SPLASHPACKStreamTable* table, const SPLASHPACKStreamRegion* regions) {
    m_table = table;
    m_regions = regions;
    m_count = 0;
    m_pendingRegion = kNone;
}

bool StreamPlanner::isResident(int region) const {
    for (int i = 0; i < m_count; i++)
        if (m_slots[i].region == region) return true;
    return false;
}

uint32_t StreamPlanner::residentOffset(int region) const {
    for (int i = 0; i < m_count; i++)
        if (m_slots[i].region == region) return m_slots[i].offset;
    return 0;
}

int StreamPlanner::nextUnload(int32_t x, int32_t z) const {
    const int64_t r2 = sq(m_table->unloadRadius);
    for (int i = 0; i < m_count; i++)
        if (streamRegionDistSq(m_regions[m_slots[i].region], x, z) > r2) return m_slots[i].region;
    return kNone;
}

bool StreamPlanner::firstFit(uint32_t size, uint32_t& outOffset) const {
    if (m_count + (m_pendingRegion != kNone ? 1 : 0) >= kMaxResident) return false;
    // Few entries, so walking every slot per candidate is cheap.
    uint32_t candidate = 0;
    while (candidate + size <= m_table->poolBytes) {
        uint32_t bump = 0;
        auto check = [&](uint32_t off, uint32_t len) {
            if (candidate < off + len && off < candidate + size && off + len > bump) bump = off + len;
        };
        for (int i = 0; i < m_count; i++) check(m_slots[i].offset, m_regions[m_slots[i].region].byteSize);
        if (m_pendingRegion != kNone) check(m_pendingOffset, m_regions[m_pendingRegion].byteSize);
        if (bump == 0) {
            outOffset = candidate;
            return true;
        }
        candidate = bump;
    }
    return false;
}

int StreamPlanner::planLoad(int32_t x, int32_t z, uint32_t& outOffset, uint16_t* evicted,
                            int& evictedCount) {
    evictedCount = 0;
    const int64_t load2 = sq(m_table->loadRadius);

    int best = kNone;
    int64_t bestD = 0;
    for (int i = 0; i < m_table->regionCount; i++) {
        if (i == m_pendingRegion || isResident(i)) continue;
        int64_t d = streamRegionDistSq(m_regions[i], x, z);
        if (d > load2) continue;
        if (best == kNone || d < bestD) {
            best = i;
            bestD = d;
        }
    }
    if (best == kNone) return kNone;

    while (!firstFit(m_regions[best].byteSize, outOffset)) {
        int victim = kNone;
        int64_t victimD = load2;  // only regions the camera does not need
        for (int i = 0; i < m_count; i++) {
            int64_t d = streamRegionDistSq(m_regions[m_slots[i].region], x, z);
            if (d > victimD) {
                victim = m_slots[i].region;
                victimD = d;
            }
        }
        if (victim == kNone) return kNone;
        evict(victim);
        evicted[evictedCount++] = static_cast<uint16_t>(victim);
    }
    return best;
}

void StreamPlanner::beginPending(int region, uint32_t offset) {
    m_pendingRegion = region;
    m_pendingOffset = offset;
}

void StreamPlanner::finishPending(bool success) {
    if (m_pendingRegion == kNone) return;
    int region = m_pendingRegion;
    m_pendingRegion = kNone;
    if (success) markResident(region, m_pendingOffset);
}

void StreamPlanner::markResident(int region, uint32_t offset) {
    if (m_count >= kMaxResident) return;
    m_slots[m_count++] = {static_cast<uint16_t>(region), offset};
}

void StreamPlanner::evict(int region) {
    for (int i = 0; i < m_count; i++) {
        if (m_slots[i].region == region) {
            m_slots[i] = m_slots[--m_count];
            return;
        }
    }
}

}  // namespace psxsplash
