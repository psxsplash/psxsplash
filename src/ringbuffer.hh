#pragma once

#include <stdint.h>

// Keeps the compiler from moving memory accesses across it. tests/host/compat.hh
// supplies the MSVC spelling.
#ifndef PSXSPLASH_COMPILER_BARRIER
#define PSXSPLASH_COMPILER_BARRIER() __asm__ volatile("" ::: "memory")
#endif

namespace psxsplash {

/**
 * Lock-free single-producer / single-consumer byte ring buffer.
 *
 * One side pushes, the other pops. It is safe to have the producer run in an
 * interrupt handler while the consumer runs on the main thread (or vice versa)
 * without a lock, because:
 *   - the head index is written only by the producer, the tail only by the
 *     consumer, and each is a 32-bit aligned word (atomic load/store on MIPS);
 *   - the data byte is fully written before head is advanced ("publish last").
 *
 * `N` must be a power of two. Head and tail are free-running counters; the
 * difference `head - tail` (unsigned) is the current fill level, which sidesteps
 * the classic full-vs-empty ambiguity without wasting a slot.
 */
template <uint32_t N>
class RingBuffer {
    static_assert(N != 0 && (N & (N - 1)) == 0, "RingBuffer size must be a power of two");

  public:
    uint32_t capacity() const { return N; }
    uint32_t size() const { return m_head - m_tail; }
    uint32_t space() const { return N - (m_head - m_tail); }
    bool empty() const { return m_head == m_tail; }
    bool full() const { return (m_head - m_tail) == N; }

    // Producer side. Returns false (dropping the byte) when full.
    bool push(uint8_t b) {
        if (full()) return false;
        m_buf[m_head & (N - 1)] = b;
        // m_buf is not volatile, so without a compiler barrier the byte store
        // may be moved after the head store and the consumer can read a stale
        // byte. MIPS I is in-order, so a compiler barrier is enough.
        PSXSPLASH_COMPILER_BARRIER();
        m_head = m_head + 1;  // publish only after the byte is stored
        return true;
    }

    // Consumer side. Returns false when empty (out is left untouched).
    bool pop(uint8_t& out) {
        if (empty()) return false;
        out = m_buf[m_tail & (N - 1)];
        // Same the other way: the byte must be read before the slot is freed.
        PSXSPLASH_COMPILER_BARRIER();
        m_tail = m_tail + 1;
        return true;
    }

    void clear() {
        m_tail = m_head;
    }

  private:
    volatile uint32_t m_head = 0;  // written by producer only
    volatile uint32_t m_tail = 0;  // written by consumer only
    uint8_t m_buf[N];
};

}  // namespace psxsplash
