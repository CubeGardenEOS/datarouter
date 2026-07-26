#ifndef DATAROUTER_REGISTRY_H
#define DATAROUTER_REGISTRY_H

#include "datarouter_internal.h"

typedef struct {
    char service_name[DATAROUTER_SERVICE_NAME_MAX + 1U];
    int32_t pid;
    uint32_t uid;
    uint64_t process_start_id;
    uint32_t route_id;
    uint32_t slot_index;
    uint64_t last_active_ns;
    unsigned int state;
} datarouter_registry_metadata;

int datarouter_registry_entry_alive(const datarouter_registry_entry *entry);
int datarouter_registry_read_active(
    const datarouter_registry_entry *entry,
    datarouter_registry_metadata *out
);
int datarouter_registry_read_routable(
    const datarouter_registry_entry *entry,
    datarouter_registry_metadata *out
);
int datarouter_registry_read_source_visible(
    const datarouter_registry_entry *entry,
    datarouter_registry_metadata *out
);
int datarouter_registry_find_active_metadata_by_name(
    datarouter_shared_memory *shm,
    const char *service_name,
    datarouter_registry_metadata *out
);
int datarouter_registry_find_active_metadata_by_route(
    datarouter_shared_memory *shm,
    uint32_t route_id,
    datarouter_registry_metadata *out
);
int datarouter_registry_find_source_metadata_by_route(
    datarouter_shared_memory *shm,
    uint32_t route_id,
    datarouter_registry_metadata *out
);
void datarouter_registry_track_inflight(
    datarouter_shared_memory *shm,
    uint32_t source_slot,
    uint32_t target_slot
);
void datarouter_registry_complete_inflight(
    datarouter_shared_memory *shm,
    uint32_t source_route,
    uint32_t target_slot
);
int datarouter_registry_publish_control_response(
    datarouter_shared_memory *shm,
    datarouter_register_request *request,
    unsigned int terminal_state,
    uint32_t operation,
    int result_code,
    uint32_t assigned_slot
);
void datarouter_registry_store_name(datarouter_registry_entry *entry, const char *service_name);
void datarouter_registry_copy_name(
    const datarouter_registry_entry *entry,
    char out[DATAROUTER_SERVICE_NAME_MAX + 1U]
);
int datarouter_registry_pin_self(
    datarouter_registry_entry *entry,
    int32_t pid,
    uint32_t uid,
    uint64_t process_start_id,
    uint32_t route_id,
    const char *service_name
);
void datarouter_registry_unpin(datarouter_registry_entry *entry);
int datarouter_registry_empty_slot_ready(datarouter_registry_entry *entry);
void datarouter_registry_clear_entry(datarouter_registry_entry *entry, uint32_t slot_index);
int datarouter_registry_release_slot(datarouter_shared_memory *shm, uint32_t slot_index);
int datarouter_registry_begin_drain(datarouter_shared_memory *shm, uint32_t slot_index);
void datarouter_registry_cleanup_releasing(datarouter_shared_memory *shm);
int datarouter_registry_release_dead_slot_locked(datarouter_shared_memory *shm, uint32_t slot_index);
void datarouter_registry_cleanup_dead(datarouter_shared_memory *shm);

datarouter_registry_entry *datarouter_registry_find_live_by_name(
    datarouter_shared_memory *shm,
    const char *service_name
);
datarouter_registry_entry *datarouter_registry_find_any_by_name(
    datarouter_shared_memory *shm,
    const char *service_name
);
datarouter_registry_entry *datarouter_registry_find_active_by_name(
    datarouter_shared_memory *shm,
    const char *service_name
);
datarouter_registry_entry *datarouter_registry_find_live_by_route(
    datarouter_shared_memory *shm,
    uint32_t route_id
);
datarouter_registry_entry *datarouter_registry_find_any_by_route(
    datarouter_shared_memory *shm,
    uint32_t route_id
);
datarouter_registry_entry *datarouter_registry_find_active_by_route(
    datarouter_shared_memory *shm,
    uint32_t route_id
);
datarouter_registry_entry *datarouter_registry_find_live_by_slot(
    datarouter_shared_memory *shm,
    uint32_t slot_index
);
datarouter_registry_entry *datarouter_registry_find_active_by_slot(
    datarouter_shared_memory *shm,
    uint32_t slot_index
);
datarouter_registry_entry *datarouter_registry_find_routable_by_slot(
    datarouter_shared_memory *shm,
    uint32_t slot_index
);

#endif
