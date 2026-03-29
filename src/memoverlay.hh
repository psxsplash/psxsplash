#pragma once

// Runtime memory overlay - shows heap/RAM usage as a progress bar + text.
// Compiled only when PSXSPLASH_MEMOVERLAY is defined (set from SplashEdit build flag).

#ifdef PSXSPLASH_MEMOVERLAY

#include <psyqo/font.hh>
#include <psyqo/gpu.hh>
#include <psyqo/bump-allocator.hh>
#include <psyqo/ordering-table.hh>

#include "renderer.hh"

namespace psxsplash {

class MemOverlay {
public:
    /// Call once at startup to cache linker symbol addresses.
    void init(psyqo::Font<>* font);

    /// Phase 1: Insert progress bar rectangles into the OT.
    /// Call BEFORE gpu.chain(ot).
    void renderOT(psyqo::OrderingTable<Renderer::ORDERING_TABLE_SIZE>& ot,
                  psyqo::BumpAllocator<Renderer::BUMP_ALLOCATOR_SIZE>& balloc);

    /// Phase 2: Emit text via chainprintf.
    /// Call AFTER gpu.chain(ot).
    void renderText(psyqo::GPU& gpu);

private:
    psyqo::Font<>* m_font = nullptr;

    // Cached addresses from linker symbols
    uintptr_t m_heapBase = 0;
    uintptr_t m_stackTop = 0;
    uint32_t  m_totalAvail = 0;

    // Cached each frame for text phase
    uint32_t m_lastUsedKB = 0;
    uint32_t m_lastTotalKB = 0;
    uint32_t m_lastPct = 0;
};

} // namespace psxsplash

#endif // PSXSPLASH_MEMOVERLAY
