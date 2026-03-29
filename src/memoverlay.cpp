#ifdef PSXSPLASH_MEMOVERLAY

#include "memoverlay.hh"

#include <psyqo/alloc.h>
#include <psyqo/primitives/rectangles.hh>

#include "vram_config.h"

// Linker symbols
extern "C" {
extern char __heap_start;
extern char __stack_start;
}

namespace psxsplash {

void MemOverlay::init(psyqo::Font<>* font) {
    m_font = font;
    m_heapBase = reinterpret_cast<uintptr_t>(&__heap_start);
    m_stackTop = reinterpret_cast<uintptr_t>(&__stack_start);
    m_totalAvail = (uint32_t)(m_stackTop - m_heapBase);
}

void MemOverlay::renderOT(psyqo::OrderingTable<Renderer::ORDERING_TABLE_SIZE>& ot,
                           psyqo::BumpAllocator<Renderer::BUMP_ALLOCATOR_SIZE>& balloc) {
    if (!m_font || m_totalAvail == 0) return;

    // Measure current heap usage
    void* heapEnd = psyqo_heap_end();
    uint32_t heapUsed = 0;
    if (heapEnd != nullptr) {
        heapUsed = (uint32_t)(reinterpret_cast<uintptr_t>(heapEnd) - m_heapBase);
    }

    m_lastPct = (heapUsed * 100) / m_totalAvail;
    if (m_lastPct > 100) m_lastPct = 100;
    m_lastUsedKB = heapUsed / 1024;
    m_lastTotalKB = m_totalAvail / 1024;

    // Bar dimensions - top-right corner
    static constexpr int16_t BAR_W = 80;
    static constexpr int16_t BAR_H = 6;
    static constexpr int16_t BAR_X = VRAM_RES_WIDTH - BAR_W - 4;
    static constexpr int16_t BAR_Y = 4;

    // Need room for two rectangles
    size_t needed = sizeof(psyqo::Fragments::SimpleFragment<psyqo::Prim::Rectangle>) * 2 + 32;
    if (balloc.remaining() < needed) return;

    // Fill bar first (OT is LIFO, so first insert renders last / on top)
    int16_t fillW = (int16_t)((uint32_t)BAR_W * m_lastPct / 100);
    if (fillW > 0) {
        uint8_t r, g;
        if (m_lastPct < 50) {
            r = (uint8_t)(m_lastPct * 5);
            g = 200;
        } else {
            r = 250;
            g = (uint8_t)((100 - m_lastPct) * 4);
        }

        auto& fillFrag = balloc.allocateFragment<psyqo::Prim::Rectangle>();
        fillFrag.primitive.setColor(psyqo::Color{.r = r, .g = g, .b = 20});
        fillFrag.primitive.position = {.x = BAR_X, .y = BAR_Y};
        fillFrag.primitive.size = {.x = fillW, .y = BAR_H};
        fillFrag.primitive.setOpaque();
        ot.insert(fillFrag, 0);
    }

    // Background bar (inserted second, so renders first / behind fill)
    auto& bgFrag = balloc.allocateFragment<psyqo::Prim::Rectangle>();
    bgFrag.primitive.setColor(psyqo::Color{.r = 40, .g = 40, .b = 40});
    bgFrag.primitive.position = {.x = BAR_X, .y = BAR_Y};
    bgFrag.primitive.size = {.x = BAR_W, .y = BAR_H};
    bgFrag.primitive.setOpaque();
    ot.insert(bgFrag, 0);
}

void MemOverlay::renderText(psyqo::GPU& gpu) {
    if (!m_font || m_totalAvail == 0) return;

    static constexpr int16_t BAR_H = 6;
    static constexpr int16_t BAR_X = VRAM_RES_WIDTH - 80 - 4;
    static constexpr int16_t BAR_Y = 4;
    static constexpr int16_t TEXT_Y = BAR_Y + BAR_H + 2;

    m_font->chainprintf(gpu,
        {{.x = BAR_X - 20, .y = TEXT_Y}},
        {{.r = 0xcc, .g = 0xcc, .b = 0xcc}},
        "%luK/%luK", m_lastUsedKB, m_lastTotalKB);
}

} // namespace psxsplash

#endif // PSXSPLASH_MEMOVERLAY
