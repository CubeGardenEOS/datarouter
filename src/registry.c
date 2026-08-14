#include "datarouter_registry.h"
#include "datarouter_scheduler.h"
#include "datarouter_signal.h"

#include <stdatomic.h>
#include <string.h>

static int state_is_routable(unsigned int state) {
    return state == DATAROUTER_PORT_ACTIVE || state == DATAROUTER_PORT_DRAINING;
}

void datarouter_registry_store_name(datarouter_registry_entry *entry, const char *service_name) {
    if (!entry) {
        return;
    }
    for (size_t i = 0U; i <= DATAROUTER_SERVICE_NAME_MAX; i++) {
        unsigned char value = service_name ? (unsigned char)service_name[i] : 0U;
        atomic_store_explicit(&entry->service_name[i], value, memory_order_relaxed);
        if (value == 0U) {
            for (size_t j = i + 1U; j <= DATAROUTER_SERVICE_NAME_MAX; j++) {
                atomic_store_explicit(&entry->service_name[j], 0U, memory_order_relaxed);
            }
            return;
        }
    }
    atomic_store_explicit(
        &entry->service_name[DATAROUTER_SERVICE_NAME_MAX],
        0U,
        memory_order_relaxed
    );
}

void datarouter_registry_copy_name(
    const datarouter_registry_entry *entry,
    char out[DATAROUTER_SERVICE_NAME_MAX + 1U]
) {
    if (!out) {
        return;
    }
    memset(out, 0, DATAROUTER_SERVICE_NAME_MAX + 1U);
    if (!entry) {
        return;
    }
    for (size_t i = 0U; i < DATAROUTER_SERVICE_NAME_MAX; i++) {
        unsigned char value = atomic_load_explicit(&entry->service_name[i], memory_order_relaxed);
        out[i] = (char)value;
        if (value == 0U) {
            return;
        }
    }
}

static int registry_read_state(
    const datarouter_registry_entry *entry,
    unsigned int visibility,
    datarouter_registry_metadata *out
) {
    if (!entry || !out) {
        return 0;
    }
    for (unsigned int attempt = 0U; attempt < 3U; attempt++) {
        unsigned int state = atomic_load_explicit(&entry->state, memory_order_acquire);
        if (state != DATAROUTER_PORT_ACTIVE &&
            !(visibility >= 1U && state_is_routable(state)) &&
            !(visibility >= 2U && state == DATAROUTER_PORT_RELEASING)) {
            return 0;
        }
        uint32_t route_before = atomic_load_explicit(&entry->route_id, memory_order_acquire);
        datarouter_registry_metadata value = {
            .pid = atomic_load_explicit(&entry->pid, memory_order_relaxed),
            .uid = atomic_load_explicit(&entry->uid, memory_order_relaxed),
            .process_start_id = atomic_load_explicit(
                &entry->process_start_id,
                memory_order_relaxed
            ),
            .route_id = route_before,
            .slot_index = entry->slot_index,
            .last_active_ns = atomic_load_explicit(&entry->last_active_ns, memory_order_relaxed),
            .state = state,
        };
        datarouter_registry_copy_name(entry, value.service_name);
        uint32_t route_after = atomic_load_explicit(&entry->route_id, memory_order_acquire);
        unsigned int state_after = atomic_load_explicit(&entry->state, memory_order_acquire);
        if (route_before == route_after && state == state_after) {
            *out = value;
            return route_before != 0U;
        }
    }
    return 0;
}

int datarouter_registry_read_active(
    const datarouter_registry_entry *entry,
    datarouter_registry_metadata *out
) {
    return registry_read_state(entry, 0U, out);
}

int datarouter_registry_read_routable(
    const datarouter_registry_entry *entry,
    datarouter_registry_metadata *out
) {
    return registry_read_state(entry, 1U, out);
}

int datarouter_registry_read_source_visible(
    const datarouter_registry_entry *entry,
    datarouter_registry_metadata *out
) {
    return registry_read_state(entry, 2U, out);
}

int datarouter_registry_find_active_metadata_by_name(
    datarouter_shared_memory *shm,
    const char *service_name,
    datarouter_registry_metadata *out
) {
    if (!shm || !service_name || !out) {
        return 0;
    }
    uint32_t max_ports = datarouter_active_max_ports(shm);
    for (uint32_t i = 0U; i < max_ports; i++) {
        datarouter_registry_metadata metadata;
        if (datarouter_registry_read_active(&shm->control.registry[i], &metadata) &&
            strcmp(metadata.service_name, service_name) == 0) {
            *out = metadata;
            return 1;
        }
    }
    return 0;
}

int datarouter_registry_find_active_metadata_by_route(
    datarouter_shared_memory *shm,
    uint32_t route_id,
    datarouter_registry_metadata *out
) {
    if (!shm || route_id == 0U || !out) {
        return 0;
    }
    uint32_t max_ports = datarouter_active_max_ports(shm);
    for (uint32_t i = 0U; i < max_ports; i++) {
        datarouter_registry_metadata metadata;
        if (datarouter_registry_read_active(&shm->control.registry[i], &metadata) &&
            metadata.route_id == route_id) {
            *out = metadata;
            return 1;
        }
    }
    return 0;
}

int datarouter_registry_find_source_metadata_by_route(
    datarouter_shared_memory *shm,
    uint32_t route_id,
    datarouter_registry_metadata *out
) {
    if (!shm || route_id == 0U || !out) {
        return 0;
    }
    uint32_t slot = route_id & DATAROUTER_ROUTE_SLOT_MASK;
    if (slot >= datarouter_active_max_ports(shm)) {
        return 0;
    }
    datarouter_registry_metadata metadata;
    if (!datarouter_registry_read_source_visible(&shm->control.registry[slot], &metadata) ||
        metadata.route_id != route_id) {
        return 0;
    }
    *out = metadata;
    return 1;
}

void datarouter_registry_track_inflight(
    datarouter_shared_memory *shm,
    uint32_t source_slot,
    uint32_t target_slot
) {
    if (!shm || source_slot >= datarouter_active_max_ports(shm) ||
        target_slot >= datarouter_active_max_ports(shm)) {
        return;
    }
    atomic_fetch_add_explicit(
        &shm->control.inflight_frames[source_slot][target_slot],
        1U,
        memory_order_release
    );
}

void datarouter_registry_complete_inflight(
    datarouter_shared_memory *shm,
    uint32_t source_route,
    uint32_t target_slot
) {
    if (!shm || source_route == 0U || target_slot >= datarouter_active_max_ports(shm)) {
        return;
    }
    uint32_t source_slot = source_route & DATAROUTER_ROUTE_SLOT_MASK;
    if (source_slot >= datarouter_active_max_ports(shm)) {
        return;
    }
    /* A source slot cannot be reused while this counter is non-zero, so the
     * route's slot bits are sufficient here and avoid a registry snapshot on
     * every received frame. */
    atomic_uint *counter = &shm->control.inflight_frames[source_slot][target_slot];
    unsigned int current = atomic_load_explicit(counter, memory_order_acquire);
    while (current > 0U &&
           !atomic_compare_exchange_weak_explicit(
               counter,
               &current,
               current - 1U,
               memory_order_acq_rel,
               memory_order_acquire
           )) {
    }
}

int datarouter_registry_entry_alive(const datarouter_registry_entry *entry) {
    datarouter_registry_metadata metadata;
    if (!datarouter_registry_read_routable(entry, &metadata)) {
        return 0;
    }
    return datarouter_process_identity_matches(
        (pid_t)metadata.pid,
        metadata.uid,
        metadata.process_start_id
    );
}

int datarouter_registry_pin_self(
    datarouter_registry_entry *entry,
    int32_t pid,
    uint32_t uid,
    uint64_t process_start_id,
    uint32_t route_id,
    const char *service_name
) {
    if (!entry || !service_name) {
        return 0;
    }
    /* io_users and state form a two-atomic exclusion handshake with the
     * switch. Sequential consistency ensures either this pin observes the
     * closed state or the closer observes this reference before resetting the
     * rings; acquire/release on separate atomics is insufficient here. */
    atomic_fetch_add_explicit(&entry->io_users, 1U, memory_order_seq_cst);
    datarouter_registry_metadata metadata;
    int matches = atomic_load_explicit(&entry->state, memory_order_seq_cst) ==
                      DATAROUTER_PORT_ACTIVE &&
                  datarouter_registry_read_active(entry, &metadata) && metadata.pid == pid &&
                  metadata.uid == uid && metadata.process_start_id == process_start_id &&
                  metadata.route_id == route_id &&
                  strcmp(metadata.service_name, service_name) == 0 &&
                  atomic_load_explicit(&entry->state, memory_order_seq_cst) ==
                      DATAROUTER_PORT_ACTIVE;
    if (!matches) {
        atomic_fetch_sub_explicit(&entry->io_users, 1U, memory_order_seq_cst);
    }
    return matches;
}

void datarouter_registry_unpin(datarouter_registry_entry *entry) {
    if (entry) {
        atomic_fetch_sub_explicit(&entry->io_users, 1U, memory_order_seq_cst);
    }
}

int datarouter_registry_empty_slot_ready(datarouter_registry_entry *entry) {
    if (!entry ||
        atomic_load_explicit(&entry->state, memory_order_seq_cst) != DATAROUTER_PORT_EMPTY) {
        return 0;
    }
    if (atomic_load_explicit(&entry->io_users, memory_order_seq_cst) == 0U) {
        return 1;
    }
    int32_t pid = atomic_load_explicit(&entry->pid, memory_order_relaxed);
    uint32_t uid = atomic_load_explicit(&entry->uid, memory_order_relaxed);
    uint64_t start_id = atomic_load_explicit(&entry->process_start_id, memory_order_relaxed);
    if (pid > 0 && datarouter_process_identity_matches((pid_t)pid, uid, start_id)) {
        return 0;
    }
    atomic_store_explicit(&entry->io_users, 0U, memory_order_seq_cst);
    return 1;
}

void datarouter_registry_clear_entry(datarouter_registry_entry *entry, uint32_t slot_index) {
    if (!entry) {
        return;
    }
    uint32_t generation = entry->generation;
    datarouter_registry_store_name(entry, NULL);
    entry->slot_index = slot_index;
    entry->generation = generation;
    atomic_store_explicit(
        &entry->route_id,
        datarouter_route_for_slot_generation(slot_index, generation),
        memory_order_relaxed
    );
    atomic_store_explicit(&entry->last_active_ns, 0ULL, memory_order_release);
    atomic_store_explicit(&entry->io_users, 0U, memory_order_seq_cst);
    atomic_store_explicit(&entry->direct_writers, 0U, memory_order_seq_cst);
    atomic_store_explicit(&entry->direct_writer_pid, 0, memory_order_relaxed);
    atomic_store_explicit(&entry->direct_writer_uid, 0U, memory_order_relaxed);
    atomic_store_explicit(&entry->direct_writer_start_id, 0ULL, memory_order_relaxed);
    atomic_store_explicit(&entry->state, DATAROUTER_PORT_EMPTY, memory_order_seq_cst);
}

static int force_dead_owner_pin_release(datarouter_registry_entry *entry) {
    int32_t pid = atomic_load_explicit(&entry->pid, memory_order_relaxed);
    uint32_t uid = atomic_load_explicit(&entry->uid, memory_order_relaxed);
    uint64_t start_id = atomic_load_explicit(&entry->process_start_id, memory_order_relaxed);
    if (datarouter_process_identity_matches((pid_t)pid, uid, start_id)) {
        return 0;
    }
    atomic_store_explicit(&entry->io_users, 0U, memory_order_seq_cst);
    return 1;
}

/* Wait for an in-flight direct (unsafe) data-plane writer to finish.
 *
 * direct_writers is pinned by live writer processes, so unlike owner pins it
 * must NEVER be zeroed while the writer is still alive: doing so would reset
 * the rings underneath its memcpy and corrupt the next occupant's RX channel
 * (and its later unpin would underflow the counter). Instead we wait out the
 * bounded write, and only force-release when the recorded writer process is
 * provably dead (crashed mid-write). An alive-but-preempted writer simply
 * defers the finalize; the switch's cleanup loop retries. */
static int direct_writers_drain(datarouter_registry_entry *entry) {
    uint64_t started = datarouter_now_ns();
    for (;;) {
        if (atomic_load_explicit(&entry->direct_writers, memory_order_seq_cst) == 0U) {
            return 1;
        }
        int32_t pid = atomic_load_explicit(&entry->direct_writer_pid, memory_order_relaxed);
        uint32_t uid = atomic_load_explicit(&entry->direct_writer_uid, memory_order_relaxed);
        uint64_t start_id = atomic_load_explicit(
            &entry->direct_writer_start_id,
            memory_order_relaxed
        );
        if (pid <= 0 ||
            !datarouter_process_identity_matches((pid_t)pid, uid, start_id)) {
            /* The writer died before unpinning; its memcpy never completed and
             * nothing will ever decrement the counter. Clear the stale pin. */
            atomic_store_explicit(&entry->direct_writers, 0U, memory_order_seq_cst);
            atomic_store_explicit(&entry->direct_writer_pid, 0, memory_order_relaxed);
            atomic_store_explicit(&entry->direct_writer_uid, 0U, memory_order_relaxed);
            atomic_store_explicit(&entry->direct_writer_start_id, 0ULL, memory_order_relaxed);
            return 1;
        }
        if (datarouter_now_ns() - started >= DATAROUTER_DIRECT_WRITER_WAIT_NS) {
            /* Writer alive but still mid-write (or preempted). Defer the
             * release; the cleanup loop retries finalize_release. */
            return 0;
        }
        datarouter_sleep_us(100U);
    }
}

static int finalize_release(datarouter_shared_memory *shm, uint32_t slot_index) {
    datarouter_registry_entry *entry = &shm->control.registry[slot_index];
    if (atomic_load_explicit(&entry->io_users, memory_order_seq_cst) != 0U &&
        !force_dead_owner_pin_release(entry)) {
        return 0;
    }
    if (!direct_writers_drain(entry)) {
        return 0;
    }
    datarouter_scheduler_forget_slot(shm, slot_index);
    datarouter_ring_reset(datarouter_rx_channel(shm, slot_index));
    datarouter_ring_reset(datarouter_tx_channel(shm, slot_index));
    for (uint32_t source = 0U; source < datarouter_active_max_ports(shm); source++) {
        atomic_store_explicit(
            &shm->control.inflight_frames[source][slot_index],
            0U,
            memory_order_release
        );
    }
    if (datarouter_scheduler_wake_blocked(shm) > 0U) {
        datarouter_signal_post(&shm->control.switch_signal);
    }
    for (uint32_t target = 0U; target < datarouter_active_max_ports(shm); target++) {
        if (atomic_load_explicit(
                &shm->control.inflight_frames[slot_index][target],
                memory_order_acquire
            ) != 0U) {
            return 0;
        }
    }
    datarouter_registry_clear_entry(entry, slot_index);
    return 1;
}

int datarouter_registry_release_slot(datarouter_shared_memory *shm, uint32_t slot_index) {
    if (!shm || slot_index >= datarouter_active_max_ports(shm)) {
        return 0;
    }
    datarouter_registry_entry *entry = &shm->control.registry[slot_index];
    unsigned int state = atomic_load_explicit(&entry->state, memory_order_seq_cst);
    while (state == DATAROUTER_PORT_ACTIVE || state == DATAROUTER_PORT_DRAINING) {
        if (atomic_compare_exchange_weak_explicit(
                &entry->state,
                &state,
                DATAROUTER_PORT_RELEASING,
                memory_order_seq_cst,
                memory_order_seq_cst
            )) {
            break;
        }
    }
    if (state == DATAROUTER_PORT_EMPTY) {
        return 1;
    }
    if (state != DATAROUTER_PORT_RELEASING &&
        atomic_load_explicit(&entry->state, memory_order_seq_cst) != DATAROUTER_PORT_RELEASING) {
        return 0;
    }
    return finalize_release(shm, slot_index);
}

int datarouter_registry_begin_drain(datarouter_shared_memory *shm, uint32_t slot_index) {
    if (!shm || slot_index >= datarouter_active_max_ports(shm)) {
        return 0;
    }
    datarouter_registry_entry *entry = &shm->control.registry[slot_index];
    unsigned int active = DATAROUTER_PORT_ACTIVE;
    if (!atomic_compare_exchange_strong_explicit(
            &entry->state,
            &active,
            DATAROUTER_PORT_DRAINING,
            memory_order_seq_cst,
            memory_order_seq_cst
        )) {
        return active == DATAROUTER_PORT_DRAINING;
    }
    datarouter_scheduler_mark_tx_ready(shm, slot_index);
    return 1;
}

void datarouter_registry_cleanup_releasing(datarouter_shared_memory *shm) {
    if (!shm) {
        return;
    }
    uint32_t max_ports = datarouter_active_max_ports(shm);
    for (uint32_t slot = 0U; slot < max_ports; slot++) {
        datarouter_registry_entry *entry = &shm->control.registry[slot];
        unsigned int state = atomic_load_explicit(&entry->state, memory_order_seq_cst);
        if (state == DATAROUTER_PORT_RELEASING) {
            (void)finalize_release(shm, slot);
            continue;
        }
        if (state != DATAROUTER_PORT_DRAINING) {
            continue;
        }
        if (atomic_load_explicit(&entry->io_users, memory_order_seq_cst) != 0U &&
            !force_dead_owner_pin_release(entry)) {
            continue;
        }
        datarouter_channel *tx = datarouter_tx_channel(shm, slot);
        size_t used = 0U;
        int rc = datarouter_ring_validate(tx, &used);
        if (rc != DATAROUTER_OK) {
            (void)datarouter_registry_release_slot(shm, slot);
        } else if (used == 0U) {
            unsigned int draining = DATAROUTER_PORT_DRAINING;
            if (atomic_compare_exchange_strong_explicit(
                    &entry->state,
                    &draining,
                    DATAROUTER_PORT_RELEASING,
                    memory_order_seq_cst,
                    memory_order_seq_cst
                )) {
                (void)finalize_release(shm, slot);
            }
        } else {
            datarouter_scheduler_mark_tx_ready(shm, slot);
        }
    }
}

int datarouter_registry_publish_control_response(
    datarouter_shared_memory *shm,
    datarouter_register_request *request,
    unsigned int terminal_state,
    uint32_t operation,
    int result_code,
    uint32_t assigned_slot
) {
    if (!shm || !request ||
        (terminal_state != DATAROUTER_REG_ACCEPTED &&
         terminal_state != DATAROUTER_REG_REJECTED)) {
        return -1;
    }
    unsigned int processing = DATAROUTER_REG_PROCESSING;
    if (atomic_compare_exchange_strong_explicit(
            &request->state,
            &processing,
            terminal_state,
            memory_order_release,
            memory_order_acquire
        )) {
        return 1;
    }
    if (processing != DATAROUTER_REG_ABANDONED) {
        return -1;
    }

    /* A timed-out REGISTER caller never learned its route. Roll back the new
     * slot before releasing the mailbox so a retry cannot leave or encounter
     * a ghost service. A late UNREGISTER keeps its requested drain side effect. */
    if (operation == DATAROUTER_REG_OP_REGISTER && result_code == DATAROUTER_OK &&
        assigned_slot < datarouter_active_max_ports(shm)) {
        (void)datarouter_registry_release_slot(shm, assigned_slot);
    }
    atomic_store_explicit(&request->state, DATAROUTER_REG_EMPTY, memory_order_release);
    atomic_store_explicit(&request->claim_token, 0ULL, memory_order_release);
    atomic_fetch_add_explicit(
        &shm->control.registration_request_recoveries,
        1ULL,
        memory_order_relaxed
    );
    return 0;
}

int datarouter_registry_release_dead_slot_locked(datarouter_shared_memory *shm, uint32_t slot_index) {
    if (!shm || !datarouter_try_lock(&shm->control.control_lock)) {
        return 0;
    }
    int released = 0;
    if (slot_index < datarouter_active_max_ports(shm)) {
        datarouter_registry_entry *entry = &shm->control.registry[slot_index];
        if (atomic_load_explicit(&entry->state, memory_order_acquire) == DATAROUTER_PORT_ACTIVE &&
            !datarouter_registry_entry_alive(entry)) {
            released = datarouter_registry_release_slot(shm, slot_index);
        }
    }
    datarouter_unlock(&shm->control.control_lock);
    return released;
}

void datarouter_registry_cleanup_dead(datarouter_shared_memory *shm) {
    if (!shm || !datarouter_try_lock(&shm->control.control_lock)) {
        return;
    }
    uint32_t max_ports = datarouter_active_max_ports(shm);
    for (uint32_t i = 0U; i < max_ports; i++) {
        datarouter_registry_entry *entry = &shm->control.registry[i];
        if (atomic_load_explicit(&entry->state, memory_order_acquire) == DATAROUTER_PORT_ACTIVE &&
            !datarouter_registry_entry_alive(entry)) {
            (void)datarouter_registry_release_slot(shm, i);
        }
    }
    datarouter_unlock(&shm->control.control_lock);
}

datarouter_registry_entry *datarouter_registry_find_live_by_name(
    datarouter_shared_memory *shm,
    const char *service_name
) {
    datarouter_registry_entry *entry = datarouter_registry_find_any_by_name(shm, service_name);
    return datarouter_registry_entry_alive(entry) ? entry : NULL;
}

datarouter_registry_entry *datarouter_registry_find_any_by_name(
    datarouter_shared_memory *shm,
    const char *service_name
) {
    if (!shm || !service_name) {
        return NULL;
    }
    uint32_t max_ports = datarouter_active_max_ports(shm);
    for (uint32_t i = 0U; i < max_ports; i++) {
        datarouter_registry_entry *entry = &shm->control.registry[i];
        datarouter_registry_metadata metadata;
        if (datarouter_registry_read_active(entry, &metadata) &&
            strcmp(metadata.service_name, service_name) == 0) {
            return entry;
        }
    }
    return NULL;
}

datarouter_registry_entry *datarouter_registry_find_active_by_name(
    datarouter_shared_memory *shm,
    const char *service_name
) {
    return datarouter_registry_find_any_by_name(shm, service_name);
}

datarouter_registry_entry *datarouter_registry_find_live_by_route(
    datarouter_shared_memory *shm,
    uint32_t route_id
) {
    datarouter_registry_entry *entry = datarouter_registry_find_any_by_route(shm, route_id);
    return datarouter_registry_entry_alive(entry) ? entry : NULL;
}

datarouter_registry_entry *datarouter_registry_find_any_by_route(
    datarouter_shared_memory *shm,
    uint32_t route_id
) {
    if (!shm || route_id == 0U) {
        return NULL;
    }
    uint32_t max_ports = datarouter_active_max_ports(shm);
    for (uint32_t i = 0U; i < max_ports; i++) {
        datarouter_registry_entry *entry = &shm->control.registry[i];
        datarouter_registry_metadata metadata;
        if (datarouter_registry_read_active(entry, &metadata) && metadata.route_id == route_id) {
            return entry;
        }
    }
    return NULL;
}

datarouter_registry_entry *datarouter_registry_find_active_by_route(
    datarouter_shared_memory *shm,
    uint32_t route_id
) {
    return datarouter_registry_find_any_by_route(shm, route_id);
}

datarouter_registry_entry *datarouter_registry_find_live_by_slot(
    datarouter_shared_memory *shm,
    uint32_t slot_index
) {
    datarouter_registry_entry *entry = datarouter_registry_find_active_by_slot(shm, slot_index);
    return datarouter_registry_entry_alive(entry) ? entry : NULL;
}

datarouter_registry_entry *datarouter_registry_find_active_by_slot(
    datarouter_shared_memory *shm,
    uint32_t slot_index
) {
    if (!shm) {
        return NULL;
    }
    uint32_t max_ports = datarouter_active_max_ports(shm);
    if (slot_index >= max_ports) {
        return NULL;
    }
    datarouter_registry_entry *entry = &shm->control.registry[slot_index];
    datarouter_registry_metadata metadata;
    return datarouter_registry_read_active(entry, &metadata) ? entry : NULL;
}

datarouter_registry_entry *datarouter_registry_find_routable_by_slot(
    datarouter_shared_memory *shm,
    uint32_t slot_index
) {
    if (!shm || slot_index >= datarouter_active_max_ports(shm)) {
        return NULL;
    }
    datarouter_registry_entry *entry = &shm->control.registry[slot_index];
    datarouter_registry_metadata metadata;
    return datarouter_registry_read_routable(entry, &metadata) ? entry : NULL;
}
