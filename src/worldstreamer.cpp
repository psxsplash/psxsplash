#include "worldstreamer.hh"

#include <psyqo/xprintf.h>

#include "fileloader.hh"

#if defined(LOADER_CDROM)
#include "cdromhelper.hh"
#endif

namespace psxsplash {

void WorldStreamer::init(uint8_t* data, uint32_t tableOffset, int sceneIndex,
                         eastl::vector<GameObject*>& objects, int32_t startX, int32_t startZ) {
    shutdown();
    if (tableOffset == 0) return;

    auto* table = reinterpret_cast<const SPLASHPACKStreamTable*>(data + tableOffset);
    if (table->regionCount == 0 || table->slotCount == 0 || table->slotBytes == 0) return;

    char name[32];
    FileLoader::BuildGeoFilename(sceneIndex, name, sizeof(name));

#if defined(LOADER_CDROM)
    // InitializeScene runs after loadScene has silenced the drive.
    CDRomHelper::WakeDrive();
#endif
    if (!StreamReader::Open(name, m_file)) {
        printf("WorldStreamer: %s missing, streamed geometry will not draw\n", name);
#if defined(LOADER_CDROM)
        CDRomHelper::SilenceDrive();
#endif
        return;
    }

    m_table = table;
    m_regions = reinterpret_cast<const SPLASHPACKStreamRegion*>(table + 1);
    m_refs = reinterpret_cast<const SPLASHPACKStreamObjectRef*>(m_regions + table->regionCount);
    m_objects = objects.data();
    m_objectCount = static_cast<uint16_t>(objects.size());
    // The only allocation streaming makes; every region is read into a slot of it.
    m_pool = new uint8_t[static_cast<uint32_t>(table->slotCount) * table->slotBytes];
    m_planner.reset(m_table, m_regions);
    // The drive is ours for the scene: Audio.PlayCDDA is refused from here on.
    StreamReader::Get().reserveDrive(true);

    // Everything in range of the spawn point loads now, nearest first, so the
    // first frame is not missing geometry.
    uint16_t evicted[StreamPlanner::kMaxResident];
    int evictedCount;
    uint32_t off;
    int region;
    while ((region = m_planner.planLoad(startX, startZ, off, evicted, evictedCount)) != StreamPlanner::kNone) {
        for (int i = 0; i < evictedCount; i++) detach(evicted[i]);
        const auto& r = m_regions[region];
        if (!StreamReader::ReadBlocking(m_file, r.firstSector, r.byteSize / 2048, m_pool + off)) {
            printf("WorldStreamer: read of region %d failed\n", region);
            break;
        }
        m_planner.markResident(region, off);
        attach(region, off);
    }

#if defined(LOADER_CDROM)
    CDRomHelper::SilenceDrive();
#endif
}

void WorldStreamer::shutdown() {
    m_generation++;
    if (m_table) {
        while (m_planner.residentCount() > 0) {
            int region = m_planner.residentRegion(0);
            m_planner.evict(region);
            detach(region);
        }
    }
    m_planner.reset(nullptr, nullptr);
    delete[] m_pool;
    m_pool = nullptr;
    m_table = nullptr;
    m_regions = nullptr;
    m_refs = nullptr;
    m_objects = nullptr;
    m_objectCount = 0;
    m_file = StreamFile{};
}

void WorldStreamer::attach(int region, uint32_t poolOffset) {
    const auto& r = m_regions[region];
    for (uint16_t k = 0; k < r.objectRefCount; k++) {
        const auto& ref = m_refs[r.firstObjectRef + k];
        if (ref.objectIndex >= m_objectCount) continue;
        GameObject* go = m_objects[ref.objectIndex];
        go->polygons = reinterpret_cast<Tri*>(m_pool + poolOffset + ref.offset);
        go->polyCount = ref.polyCount;
    }
#ifdef PSXSPLASH_STREAM_LOG
    printf("WORLDSTREAM attach region=%d resident=%d\n", region, m_planner.residentCount());
#endif
}

void WorldStreamer::detach(int region) {
    const auto& r = m_regions[region];
    for (uint16_t k = 0; k < r.objectRefCount; k++) {
        const auto& ref = m_refs[r.firstObjectRef + k];
        if (ref.objectIndex >= m_objectCount) continue;
        GameObject* go = m_objects[ref.objectIndex];
        go->polyCount = 0;  // every draw path skips a zero-count object
        go->polygons = nullptr;
    }
#ifdef PSXSPLASH_STREAM_LOG
    printf("WORLDSTREAM detach region=%d resident=%d\n", region, m_planner.residentCount());
#endif
}

void WorldStreamer::update(int32_t camX, int32_t camZ) {
    if (!m_table) return;

    int region;
    while ((region = m_planner.nextUnload(camX, camZ)) != StreamPlanner::kNone) {
        m_planner.evict(region);
        detach(region);
    }

    if (m_planner.hasPending()) return;  // one region in flight at a time

    uint16_t evicted[StreamPlanner::kMaxResident];
    int evictedCount;
    uint32_t off;
    region = m_planner.planLoad(camX, camZ, off, evicted, evictedCount);
    for (int i = 0; i < evictedCount; i++) detach(evicted[i]);
    if (region == StreamPlanner::kNone) return;

    const auto& r = m_regions[region];
    uint32_t gen = m_generation;
    m_planner.beginPending(region, off);
    bool queued = StreamReader::Get().request(m_file, r.firstSector, r.byteSize / 2048, m_pool + off,
                                              [this, gen, region, off](bool ok) {
        if (gen != m_generation) return;  // scene changed underneath us
        m_planner.finishPending(ok);
        if (ok) attach(region, off);
        // On failure the region stays absent and is retried on a later frame.
    });
    if (!queued) m_planner.finishPending(false);
}

}  // namespace psxsplash
