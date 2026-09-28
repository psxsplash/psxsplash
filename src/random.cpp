#include "random.hh"

// xorshift based rand
uint32_t Random::rand() {
    uint32_t x = m_seed;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    m_seed = x;
    return x;
}

// INITIAL_SEED is odd, so the product is 0 only for seed 0, and a zero state
// would make xorshift return 0 forever.
void Random::seed(uint32_t seed) { m_seed = seed ? INITIAL_SEED * seed : INITIAL_SEED; }