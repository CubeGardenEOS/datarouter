#ifndef DATAROUTER_ROUTER_H
#define DATAROUTER_ROUTER_H

#include "datarouter_internal.h"

typedef struct {
    uint64_t routed;
    uint64_t full;
    uint64_t not_found;
    uint64_t protocol;
    uint64_t internal;
    uint64_t corrupted;
} datarouter_route_stats;

uint32_t datarouter_route_frames(datarouter_shared_memory *shm);
uint32_t datarouter_route_frames_stats(datarouter_shared_memory *shm, datarouter_route_stats *stats);

#ifdef DATAROUTER_TEST_HOOKS
extern void (*datarouter_test_after_full_before_block)(
    datarouter_shared_memory *shm,
    uint32_t target_slot
);
extern void (*datarouter_test_after_rx_publish)(
    datarouter_shared_memory *shm,
    uint32_t source_route,
    uint32_t target_slot
);
#endif
uint32_t datarouter_route_frames_budget(
    datarouter_shared_memory *shm,
    datarouter_route_stats *stats,
    uint32_t max_frames,
    uint32_t budget_us
);

#endif
