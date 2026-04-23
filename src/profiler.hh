#pragma once

#include <stdint.h>

#ifdef PSXSPLASH_PROFILER

#include <psyqo/bump-allocator.hh>
#include <psyqo/font.hh>
#include <psyqo/gpu.hh>
#include <psyqo/ordering-table.hh>

#include "renderer.hh"

namespace psxsplash::debug {

enum ProfilerSection {
  PROFILER_ANIMATION,
  PROFILER_RENDERING,
  PROFILER_COLLISION,
  PROFILER_LUA,
  PROFILER_CONTROLS,
  PROFILER_NAVMESH,
  PROFILER_SECTION_COUNT,
};

class Profiler {
public:
  static Profiler& getInstance() {
    static Profiler instance;
    return instance;
  }

  void initialize(psyqo::Font<>* font = nullptr);
  void reset();
  void endFrame(uint32_t frameTime);

  void renderOT(psyqo::OrderingTable<Renderer::ORDERING_TABLE_SIZE>& ot,
                psyqo::BumpAllocator<Renderer::BUMP_ALLOCATOR_SIZE>& balloc);
  void renderText(psyqo::GPU& gpu);

  void setSectionTime(ProfilerSection section, uint32_t time) {
    if (section < PROFILER_SECTION_COUNT) {
      m_pendingSectionTimes[section] = time;
    }
  }

private:
  Profiler() = default;
  ~Profiler() = default;

  Profiler(const Profiler&) = delete;
  Profiler& operator=(const Profiler&) = delete;
  Profiler(Profiler&&) = delete;
  Profiler& operator=(Profiler&&) = delete;

  psyqo::Font<>* m_font = nullptr;
  bool m_registered = false;

  uint32_t m_sectionTimes[PROFILER_SECTION_COUNT] = {0};
  uint32_t m_pendingSectionTimes[PROFILER_SECTION_COUNT] = {0};
  uint32_t m_totalFrameTime = 0;
  uint32_t m_otherTime = 0;
};

} // namespace psxsplash::debug

#endif // PSXSPLASH_PROFILER
