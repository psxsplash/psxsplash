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

void Random::seed(uint32_t seed) { m_seed = INITIAL_SEED * seed; }

void Random::multiplySeed(uint32_t multi){
    m_seed *= multi;
}