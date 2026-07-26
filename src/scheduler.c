#include "datarouter_scheduler.h"

#include <stdatomic.h>

void datarouter_scheduler_clear(datarouter_shared_memory *shm) {
    if (!shm) {
        return;
    }
    atomic_store_explicit(&shm->control.tx_ready_mask, 0ULL, memory_order_release);
    atomic_store_explicit(&shm->control.tx_blocked_mask, 0ULL, memory_order_release);
    atomic_store_explicit(&shm->control.scheduler_cursor, 0U, memory_order_release);
}

void datarouter_scheduler_mark_tx_ready(datarouter_shared_memory *shm, uint32_t slot_index) {
    if (!shm || slot_index >= datarouter_active_max_ports(shm) || slot_index >= 64U) {
        return;
    }
    atomic_fetch_or_explicit(&shm->control.tx_ready_mask, 1ULL << slot_index, memory_order_release);
}

uint32_t datarouter_scheduler_take_next_ready(datarouter_shared_memory *shm) {
    if (!shm) {
        return UINT32_MAX;
    }
    uint32_t max_ports = datarouter_active_max_ports(shm);
    if (max_ports == 0U) {
        return UINT32_MAX;
    }
    uint64_t active_mask = max_ports >= 64U ? UINT64_MAX : ((1ULL << max_ports) - 1ULL);
    uint64_t ready = atomic_load_explicit(&shm->control.tx_ready_mask, memory_order_acquire) & active_mask;
    if (ready == 0ULL) {
        return UINT32_MAX;
    }
    uint32_t start = atomic_load_explicit(&shm->control.scheduler_cursor, memory_order_acquire) % max_ports;
    for (uint32_t offset = 0U; offset < max_ports; offset++) {
        uint32_t slot = (start + offset) % max_ports;
        uint64_t bit = 1ULL << slot;
        if ((ready & bit) == 0ULL) {
            continue;
        }
        atomic_fetch_and_explicit(&shm->control.tx_ready_mask, ~bit, memory_order_acq_rel);
        atomic_store_explicit(&shm->control.scheduler_cursor, (slot + 1U) % max_ports, memory_order_release);
        return slot;
    }
    return UINT32_MAX;
}

void datarouter_scheduler_mark_tx_blocked(datarouter_shared_memory *shm, uint32_t slot_index) {
    if (!shm || slot_index >= datarouter_active_max_ports(shm) || slot_index >= 64U) {
        return;
    }
    atomic_fetch_or_explicit(&shm->control.tx_blocked_mask, 1ULL << slot_index, memory_order_acq_rel);
}

void datarouter_scheduler_clear_tx_blocked(datarouter_shared_memory *shm, uint32_t slot_index) {
    if (!shm || slot_index >= datarouter_active_max_ports(shm) || slot_index >= 64U) {
        return;
    }
    atomic_fetch_and_explicit(&shm->control.tx_blocked_mask, ~(1ULL << slot_index), memory_order_release);
}

int datarouter_scheduler_has_tx_blocked(datarouter_shared_memory *shm) {
    if (!shm) {
        return 0;
    }
    return atomic_load_explicit(&shm->control.tx_blocked_mask, memory_order_acquire) != 0ULL;
}

uint32_t datarouter_scheduler_wake_blocked(datarouter_shared_memory *shm) {
    if (!shm) {
        return 0U;
    }
    uint32_t max_ports = datarouter_active_max_ports(shm);
    uint64_t active_mask = max_ports >= 64U ? UINT64_MAX : ((1ULL << max_ports) - 1ULL);
    uint64_t blocked = atomic_exchange_explicit(
        &shm->control.tx_blocked_mask,
        0ULL,
        memory_order_acq_rel
    ) & active_mask;
    if (blocked == 0ULL) {
        return 0U;
    }
    atomic_fetch_or_explicit(&shm->control.tx_ready_mask, blocked, memory_order_release);
    uint32_t count = 0U;
    for (uint64_t bits = blocked; bits != 0ULL; bits &= bits - 1ULL) {
        count++;
    }
    atomic_fetch_add_explicit(&shm->control.blocked_wakeups, count, memory_order_relaxed);
    return count;
}

void datarouter_scheduler_forget_slot(datarouter_shared_memory *shm, uint32_t slot_index) {
    if (!shm || slot_index >= 64U) {
        return;
    }
    uint64_t keep = ~(1ULL << slot_index);
    atomic_fetch_and_explicit(&shm->control.tx_ready_mask, keep, memory_order_acq_rel);
    atomic_fetch_and_explicit(&shm->control.tx_blocked_mask, keep, memory_order_acq_rel);
    atomic_fetch_and_explicit(&shm->control.corrupt_slot_mask, keep, memory_order_acq_rel);
}

void datarouter_scheduler_mark_corrupt(datarouter_shared_memory *shm, uint32_t slot_index) {
    if (!shm || slot_index >= datarouter_active_max_ports(shm) || slot_index >= 64U) {
        return;
    }
    atomic_fetch_or_explicit(&shm->control.corrupt_slot_mask, 1ULL << slot_index, memory_order_release);
}

uint64_t datarouter_scheduler_take_corrupt(datarouter_shared_memory *shm) {
    if (!shm) {
        return 0ULL;
    }
    return atomic_exchange_explicit(&shm->control.corrupt_slot_mask, 0ULL, memory_order_acq_rel);
}
