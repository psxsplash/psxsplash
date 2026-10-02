#pragma once

#include <stdint.h>

#include <EASTL/vector.h>

#include "gameobject.hh"
#include "streamplanner.hh"
#include "streamreader.hh"

namespace psxsplash {

// Streamed world geometry (SplashEdit "Stream world geometry").
//
// The exporter splits static, unscripted geometry into regions on an XZ grid and
// writes each region's triangles, sector-aligned, to SCENE_N.GEO. Those objects
// are exported with polyCount = 0, so they draw nothing until their region is
// resident; the stream table restores polyCount and points `polygons` into the
// pool. Everything else - collision, nav, textures, scripts - stays resident.
//
// StreamPlanner decides what is resident and where; this class does the I/O and
// patches the game objects.
class WorldStreamer {
  public:
    /**
     * Parse the table, open the .GEO file, and load every region already in
     * range of `startX/startZ` before returning, so the first frame is complete.
     * Called from InitializeScene, before Lua runs. No-op if streamTableOffset is 0.
     */
    void init(uint8_t* splashpackData, uint32_t streamTableOffset, int sceneIndex,
              eastl::vector<GameObject*>& objects, int32_t startX, int32_t startZ);

    /** Per frame: drop far regions, queue the nearest missing one in range. */
    void update(int32_t camX, int32_t camZ);

    /** Free the pool. The caller has already drained StreamReader. */
    void shutdown();

    bool active() const { return m_table != nullptr; }
    int residentCount() const { return m_planner.residentCount(); }

  private:
    void attach(int region, uint32_t poolOffset);
    void detach(int region);

    const SPLASHPACKStreamTable* m_table = nullptr;
    const SPLASHPACKStreamRegion* m_regions = nullptr;
    const SPLASHPACKStreamObjectRef* m_refs = nullptr;
    GameObject* const* m_objects = nullptr;
    uint16_t m_objectCount = 0;

    StreamPlanner m_planner;
    StreamFile m_file;
    uint8_t* m_pool = nullptr;
    uint32_t m_generation = 0;  // bumped on shutdown so a stale callback is ignored
};

}  // namespace psxsplash
