/* CGSGL v2. Monotonic-time policy shared by the C loops and deterministic tests. */
#ifndef DATAROUTER_WAIT_POLICY_H
#define DATAROUTER_WAIT_POLICY_H
#include <stdint.h>
static inline int dr_idle_spin(uint64_t last_forward, uint64_t now, uint64_t budget) {
    return last_forward != 0 && now >= last_forward && now-last_forward < budget;
}
static inline int dr_wait_spin(uint64_t start, uint64_t now, uint64_t remaining,
                               uint64_t call_start, uint64_t timeout) {
    return now >= start && now-start < remaining &&
        (!timeout || (now >= call_start && now-call_start < timeout));
}
/* Signal waits have millisecond resolution. Round a future deadline up;
 * already due work returns zero, never an infinite signal wait. */
static inline uint32_t dr_deadline_wait_ms(uint64_t now, uint64_t deadline,
                                          uint32_t cap_ms) {
    if (deadline <= now) return 0U;
    uint64_t ms = (deadline - now) / 1000000ULL + ((deadline - now) % 1000000ULL != 0ULL);
    return ms < cap_ms ? (uint32_t)ms : cap_ms;
}
static inline uint64_t dr_maintenance_interval_ns(int pending, uint32_t idle_ms) {
    return pending ? 1000000ULL : (uint64_t)idle_ms * 1000000ULL;
}
#endif
