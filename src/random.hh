#pragma once

#include <stdint.h>

class Random {
  public:
    // Gets a 32-bits random number, except the value 0.
    uint32_t rand();

    // Gets a random number between 0 and RANGE, exclusive.
    uint32_t number(uint32_t max) {
        return rand() % max;
    }

    void seed(uint32_t seed);

    void multiplySeed(uint32_t multi);

  private:
    static constexpr uint32_t INITIAL_SEED = 2891583007UL;
    uint32_t m_seed = INITIAL_SEED;
};
