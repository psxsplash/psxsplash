#include "profiler.hh"

#ifdef PSXSPLASH_PROFILER

using namespace psxsplash::debug;

// Writes address+name to the PCSX-Redux debugger variable registry.
static void pcsxRegisterVariable(void* address, const char* name) {
    register void* a0 asm("a0") = address;
    register const char* a1 asm("a1") = name;
    __asm__ volatile("sb %0, 0x2081(%1)" : : "r"(255), "r"(0x1f800000), "r"(a0), "r"(a1));
}

void Profiler::initialize() {
  reset();

  pcsxRegisterVariable(&sectionTimes[0], "profiler.rendering");
  pcsxRegisterVariable(&sectionTimes[1], "profiler.lua");
  pcsxRegisterVariable(&sectionTimes[2], "profiler.controls");
  pcsxRegisterVariable(&sectionTimes[3], "profiler.navmesh");
}

void Profiler::reset() {
  for (auto &time : sectionTimes) {
    time = 0;
  }
}

#endif // PSXSPLASH_PROFILER


