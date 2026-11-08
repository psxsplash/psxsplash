#pragma once

#include <stdint.h>

#include <EASTL/type_traits.h>

// The renderer runs its per-triangle loops with the stack in the 1 KB
// scratchpad: a load from the scratchpad never waits on RAM, and the
// per-triangle locals and spills are a large part of what that code loads.
// Splitting and clipping need more stack than fits, so they go back to the
// RAM stack.

extern "C" void psxsplash_callOnStack(void (*fn)(void*), void* arg, uintptr_t top);

namespace psxsplash {

static constexpr uintptr_t SCRATCHPAD_BASE = 0x1f800000;
static constexpr uintptr_t SCRATCHPAD_TOP = 0x1f800400;

static inline uintptr_t currentSp() {
    uintptr_t sp;
    __asm__ volatile("move %0, $sp" : "=r"(sp));
    return sp;
}

static inline bool onScratchpad() { return (currentSp() >> 10) == (SCRATCHPAD_BASE >> 10); }

template <typename F>
static void callOnStackThunk(void* f) {
    (*static_cast<F*>(f))();
}

// Calls f() with the stack starting below top (8-byte aligned).
template <typename F>
static inline void callOnStack(uintptr_t top, F&& f) {
    using T = typename eastl::remove_reference<F>::type;
    psxsplash_callOnStack(&callOnStackThunk<T>, &f, top);
}

}  // namespace psxsplash
