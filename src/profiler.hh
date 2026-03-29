#pragma once

#include <stdint.h>

#ifdef PSXSPLASH_PROFILER

namespace psxsplash::debug {

enum ProfilerSection {
  PROFILER_RENDERING,
  PROFILER_LUA,
  PROFILER_CONTROLS,
  PROFILER_NAVMESH,
};

class Profiler {
public:
  // Singleton accessor
  static Profiler& getInstance() {
    static Profiler instance;
    return instance;
  }

  void initialize();
  void reset();

  void setSectionTime(ProfilerSection section, uint32_t time) {
    sectionTimes[section] = time;
  }

private:
  Profiler() = default;
  ~Profiler() = default;

  // Delete copy/move semantics
  Profiler(const Profiler&) = delete;
  Profiler& operator=(const Profiler&) = delete;
  Profiler(Profiler&&) = delete;
  Profiler& operator=(Profiler&&) = delete;

  uint32_t sectionTimes[4] = {0, 0, 0, 0};

};

} // namespace psxsplash::debug

#endif // PSXSPLASH_PROFILER
