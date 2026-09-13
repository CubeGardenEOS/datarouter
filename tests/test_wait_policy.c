#include "../src/wait_policy.h"
#include <assert.h>
int main(void) {
    /* Startup, maintenance ticks and empty notifications cannot arm a window. */
    assert(!dr_idle_spin(0, 1, 200000));
    uint64_t last=1000000;
    assert(dr_idle_spin(last, 1199999, 200000));
    assert(!dr_idle_spin(last, 1200000, 200000));
    for (uint64_t now=2000000; now<10000000; now+=1000000)
        assert(!dr_idle_spin(last, now, 200000));
    last=10000000; /* Only a routed frame rearms it. */
    assert(dr_idle_spin(last, last+1, 200000));
    assert(dr_wait_spin(100, 50099, 50000, 100, 1000000));
    assert(!dr_wait_spin(100, 50100, 50000, 100, 1000000));
    /* Wakes, even at a new segment start, cannot replenish zero remaining. */
    assert(!dr_wait_spin(1000000, 1000000, 0, 100, 0));
    assert(!dr_wait_spin(100, 1100, 50000, 100, 1000));
    assert(dr_wait_spin(100, 1099, 50000, 100, 1000));
    assert(!dr_wait_spin(100, 99, 50000, 100, 0));
    assert(dr_maintenance_interval_ns(0, 10) == 10000000ULL);
    assert(dr_maintenance_interval_ns(1, 10) == 1000000ULL);
    uint64_t deadline = 10000000ULL;
    /* Empty notifications re-evaluate the same deadline. */
    for (uint64_t now = 0; now < deadline; now += 1000000ULL)
        assert(dr_deadline_wait_ms(now, deadline, 10) == (deadline-now)/1000000ULL);
    assert(dr_deadline_wait_ms(deadline, deadline, 10) == 0);
    assert(dr_deadline_wait_ms(deadline+1, deadline, 10) == 0);
    assert(dr_deadline_wait_ms(0, 1000000000ULL, 10) == 10);
    assert(dr_deadline_wait_ms(9999999, deadline, 10) == 1);
    for (uint64_t budget = 50000; budget <= 200000; budget *= 2) {
        assert(dr_wait_spin(100, 100+budget-1, budget, 100, 0));
        assert(!dr_wait_spin(100, 100+budget, budget, 100, 0));
        assert(!dr_wait_spin(2000000, 2000000, 0, 100, 0));
    }
    return 0;
}
