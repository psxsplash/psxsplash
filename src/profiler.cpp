#include "profiler.hh"

#ifdef PSXSPLASH_PROFILER

#include <psyqo/trigonometry.hh>
#include <psyqo/primitives/rectangles.hh>
#include <psyqo/primitives/triangles.hh>

#include "vram_config.h"

using namespace psxsplash::debug;

namespace {

struct ProfilerSectionInfo {
    const char* variableName;
    const char* shortLabel;
    psyqo::Color color;
};

constexpr ProfilerSectionInfo kSectionInfo[PROFILER_SECTION_COUNT] = {
    {"profiler.animation", "AN", {.r = 255, .g = 196, .b = 64}},
    {"profiler.rendering", "RN", {.r = 82, .g = 169, .b = 255}},
    {"profiler.collision", "CL", {.r = 255, .g = 120, .b = 120}},
    {"profiler.lua", "LU", {.r = 138, .g = 220, .b = 112}},
    {"profiler.controls", "CT", {.r = 216, .g = 144, .b = 255}},
    {"profiler.navmesh", "NV", {.r = 104, .g = 232, .b = 220}},
};

constexpr const char* kOtherVariableName = "profiler.other";
constexpr int kTotalBuckets = PROFILER_SECTION_COUNT + 1;
constexpr uint32_t kFullCircleRaw = 2048;
constexpr uint32_t kSmoothPieSegments = 64;

constexpr int16_t PANEL_W = 152;
constexpr int16_t PANEL_H = 126;
constexpr int16_t PANEL_X = VRAM_RES_WIDTH - PANEL_W - 4;
constexpr int16_t PANEL_Y = VRAM_RES_HEIGHT - PANEL_H - 4;
constexpr int16_t PIE_CENTER_X = PANEL_X + 26;
constexpr int16_t PIE_CENTER_Y = PANEL_Y + 52;
constexpr int16_t PIE_RADIUS = 20;
constexpr int16_t LEGEND_X = PANEL_X + 62;
constexpr int16_t LEGEND_Y = PANEL_Y + 26;
constexpr int16_t SWATCH_X = LEGEND_X - 8;
constexpr int16_t TEXT_X = LEGEND_X;
constexpr int16_t TEXT_Y = LEGEND_Y - 1;
constexpr int16_t LINE_HEIGHT = 14;
constexpr int16_t TOTAL_TEXT_X = PANEL_X + 6;
constexpr int16_t TOTAL_TEXT_Y = PANEL_Y + 6;

psyqo::Trig<> s_profilerTrig;

struct PiePoint {
  int16_t x;
  int16_t y;
};

static PiePoint calculatePiePoint(uint32_t angleRaw) {
  psyqo::Angle angle;
  angle.value = static_cast<int32_t>(angleRaw & (kFullCircleRaw - 1));

  psyqo::FixedPoint<12> sinTheta = s_profilerTrig.sin(angle);
  psyqo::FixedPoint<12> cosTheta = s_profilerTrig.cos(angle);

  return {
    static_cast<int16_t>(PIE_CENTER_X + ((sinTheta.value * PIE_RADIUS) >> 12)),
    static_cast<int16_t>(PIE_CENTER_Y - ((cosTheta.value * PIE_RADIUS) >> 12)),
  };
}

} // namespace

// Writes address+name to the PCSX-Redux debugger variable registry.
static void pcsxRegisterVariable(void* address, const char* name) {
    register void* a0 asm("a0") = address;
    register const char* a1 asm("a1") = name;
    __asm__ volatile("sb %0, 0x2081(%1)" : : "r"(255), "r"(0x1f800000), "r"(a0), "r"(a1));
}

void Profiler::initialize(psyqo::Font<>* font) {
  if (font != nullptr) {
    m_font = font;
  }

  reset();

  if (m_registered) {
    return;
  }

  for (int i = 0; i < PROFILER_SECTION_COUNT; ++i) {
    pcsxRegisterVariable(&m_sectionTimes[i], kSectionInfo[i].variableName);
  }
  pcsxRegisterVariable(&m_totalFrameTime, "profiler.total");
  pcsxRegisterVariable(&m_otherTime, kOtherVariableName);
  m_registered = true;
}

void Profiler::reset() {
  for (auto &time : m_sectionTimes) {
    time = 0;
  }
  for (auto &time : m_pendingSectionTimes) {
    time = 0;
  }
  m_totalFrameTime = 0;
  m_otherTime = 0;
}

void Profiler::endFrame(uint32_t frameTime) {
  uint32_t accounted = 0;
  for (uint32_t time : m_pendingSectionTimes) {
    accounted += time;
  }

  if (frameTime < accounted) {
    frameTime = accounted;
  }

  for (int i = 0; i < PROFILER_SECTION_COUNT; ++i) {
    m_sectionTimes[i] = m_pendingSectionTimes[i];
    m_pendingSectionTimes[i] = 0;
  }

  m_totalFrameTime = frameTime;
  m_otherTime = frameTime - accounted;
}

void Profiler::renderOT(psyqo::OrderingTable<Renderer::ORDERING_TABLE_SIZE>& ot,
                        psyqo::BumpAllocator<Renderer::BUMP_ALLOCATOR_SIZE>& balloc) {
  if (m_font == nullptr || m_totalFrameTime == 0) return;

  size_t needed =
      sizeof(psyqo::Fragments::SimpleFragment<psyqo::Prim::Rectangle>) * (2 + kTotalBuckets) +
      sizeof(psyqo::Fragments::SimpleFragment<psyqo::Prim::GouraudTriangle>) * (kSmoothPieSegments + kTotalBuckets) + 64;
  if (balloc.remaining() < needed) return;

  for (int bucket = 0; bucket < kTotalBuckets; ++bucket) {
    psyqo::Color color = bucket < PROFILER_SECTION_COUNT
        ? kSectionInfo[bucket].color
        : psyqo::Color{.r = 96, .g = 96, .b = 96};

    auto& swatch = balloc.allocateFragment<psyqo::Prim::Rectangle>();
    swatch.primitive.setColor(color);
    swatch.primitive.position = {.x = SWATCH_X, .y = static_cast<int16_t>(LEGEND_Y + bucket * LINE_HEIGHT)};
    swatch.primitive.size = {.x = 5, .y = 5};
    swatch.primitive.setOpaque();
    ot.insert(swatch, 0);
  }

  uint32_t startAngle = 0;
  for (int bucket = 0; bucket < kTotalBuckets; ++bucket) {
    uint32_t bucketTime = bucket < PROFILER_SECTION_COUNT ? m_sectionTimes[bucket] : m_otherTime;
    if (bucketTime == 0) continue;

    uint32_t endAngle = startAngle + (bucketTime * kFullCircleRaw) / m_totalFrameTime;
    if (bucket == (kTotalBuckets - 1) || endAngle > kFullCircleRaw) {
      endAngle = kFullCircleRaw;
    }
    if (endAngle <= startAngle) {
      endAngle = startAngle + 1;
    }

    psyqo::Color color = bucket < PROFILER_SECTION_COUNT
        ? kSectionInfo[bucket].color
        : psyqo::Color{.r = 96, .g = 96, .b = 96};

    uint32_t angleSpan = endAngle - startAngle;
    uint32_t triCount = (angleSpan * kSmoothPieSegments + (kFullCircleRaw - 1)) / kFullCircleRaw;
    if (triCount == 0) triCount = 1;

    uint32_t prevAngle = startAngle;
    PiePoint prevPoint = calculatePiePoint(prevAngle);
    for (uint32_t triIndex = 0; triIndex < triCount; ++triIndex) {
      uint32_t nextAngle = triIndex == (triCount - 1)
          ? endAngle
          : startAngle + ((triIndex + 1) * angleSpan) / triCount;
      PiePoint nextPoint = calculatePiePoint(nextAngle);

      auto& tri = balloc.allocateFragment<psyqo::Prim::GouraudTriangle>();
      tri.primitive.pointA.x = PIE_CENTER_X;
      tri.primitive.pointA.y = PIE_CENTER_Y;
      tri.primitive.pointB.x = prevPoint.x;
      tri.primitive.pointB.y = prevPoint.y;
      tri.primitive.pointC.x = nextPoint.x;
      tri.primitive.pointC.y = nextPoint.y;
      tri.primitive.setColorA(color);
      tri.primitive.setColorB(color);
      tri.primitive.setColorC(color);
      tri.primitive.setOpaque();
      ot.insert(tri, 0);

      prevAngle = nextAngle;
      prevPoint = nextPoint;
    }

    startAngle = endAngle;
  }

  auto& pieBackdrop = balloc.allocateFragment<psyqo::Prim::Rectangle>();
  pieBackdrop.primitive.setColor(psyqo::Color{.r = 18, .g = 18, .b = 22});
  pieBackdrop.primitive.position = {.x = static_cast<int16_t>(PIE_CENTER_X - PIE_RADIUS - 2),
                                    .y = static_cast<int16_t>(PIE_CENTER_Y - PIE_RADIUS - 2)};
  pieBackdrop.primitive.size = {.x = static_cast<int16_t>(PIE_RADIUS * 2 + 4),
                                .y = static_cast<int16_t>(PIE_RADIUS * 2 + 4)};
  pieBackdrop.primitive.setOpaque();
  ot.insert(pieBackdrop, 0);

  auto& panel = balloc.allocateFragment<psyqo::Prim::Rectangle>();
  panel.primitive.setColor(psyqo::Color{.r = 10, .g = 10, .b = 10});
  panel.primitive.position = {.x = PANEL_X, .y = PANEL_Y};
  panel.primitive.size = {.x = PANEL_W, .y = PANEL_H};
  panel.primitive.setOpaque();
  ot.insert(panel, 0);
}

void Profiler::renderText(psyqo::GPU& gpu) {
  if (m_font == nullptr || m_totalFrameTime == 0) return;

  uint32_t totalMs = m_totalFrameTime / 1000;
  uint32_t totalMsFrac = (m_totalFrameTime % 1000) / 100;

  m_font->chainprintf(gpu,
                      {{.x = TOTAL_TEXT_X, .y = TOTAL_TEXT_Y}},
                      {{.r = 255, .g = 255, .b = 255}},
                      "PF %lu.%01lu ms", totalMs, totalMsFrac);

  for (int i = 0; i < PROFILER_SECTION_COUNT; ++i) {
    uint32_t time = m_sectionTimes[i];
    uint32_t pct = (time * 100u) / m_totalFrameTime;
    m_font->chainprintf(gpu,
                        {{.x = TEXT_X, .y = static_cast<int16_t>(TEXT_Y + i * LINE_HEIGHT)}},
                        kSectionInfo[i].color,
                        "%s %2lu.%01lu %2lu%%",
                        kSectionInfo[i].shortLabel,
                        time / 1000,
                        (time % 1000) / 100,
                        pct);
  }

  uint32_t otherPct = (m_otherTime * 100u) / m_totalFrameTime;
  m_font->chainprintf(gpu,
                      {{.x = TEXT_X, .y = static_cast<int16_t>(TEXT_Y + PROFILER_SECTION_COUNT * LINE_HEIGHT)}},
                      {{.r = 180, .g = 180, .b = 180}},
                      "OT %2lu.%01lu %2lu%%",
                      m_otherTime / 1000,
                      (m_otherTime % 1000) / 100,
                      otherPct);
}

#endif // PSXSPLASH_PROFILER


