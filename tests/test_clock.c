#include "../src/datarouter_internal.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
#ifdef DATAROUTER_HAS_CLOCK_CONVERSION
    assert(datarouter_clock_ticks_to_ns(1, 0) == 0);
    assert(datarouter_clock_ticks_to_ns(864000000000ULL, 10000000ULL) == 86400000000000ULL);
    uint64_t random = 123456789U;
    for (unsigned int i = 0; i < 100000U; ++i) {
        random = random * 6364136223846793005ULL + 1;
        uint64_t ticks = random;
        random = random * 6364136223846793005ULL + 1;
        uint64_t frequency = i % 2 ? random | 1U : (random % 1000000000ULL) + 1;
        __uint128_t value = (__uint128_t)ticks * 1000000000ULL / frequency;
        uint64_t expected = value > UINT64_MAX ? UINT64_MAX : (uint64_t)value;
        assert(datarouter_clock_ticks_to_ns(ticks, frequency) == expected);
    }
#endif
    puts("clock conversion tests passed");
    return 0;
}
