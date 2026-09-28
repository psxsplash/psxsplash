#pragma once

#include <stdint.h>

class Random {
  public:
    // Gets a 32-bits random number, except the value 0.
    uint32_t rand();

    // Gets a random number between 0 and RANGE, exclusive. An empty range has no
    // members, so zero is the only answer available; without this, % 0 traps.
    uint32_t number(uint32_t max) {
        if (max == 0) return 0;
        return rand() % max;
    }

    void seed(uint32_t seed);

  private:
    static constexpr uint32_t INITIAL_SEED = 2891583007UL;
    uint32_t m_seed = INITIAL_SEED;
};
