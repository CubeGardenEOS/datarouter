#ifndef DATAROUTER_FRONTEND_H
#define DATAROUTER_FRONTEND_H

#include "datarouter.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct datarouter_frontend datarouter_frontend;

#define DATAROUTER_FRONTEND_ABI_VERSION 2U

typedef struct datarouter_frontend_options {
    const char *shm_name;
    const char *service_name;
    uint32_t connect_timeout_ms;
    uint32_t recv_timeout_ms;
    uint32_t ack_timeout_ms;
    uint32_t reader_wait_ms;
    size_t app_queue_capacity;
    size_t max_message_bytes;
    size_t max_pending_acks;
    uint32_t chunk_ttl_ms;
    size_t max_reassembly_groups;
    size_t max_reassembly_bytes;
} datarouter_frontend_options;

typedef struct datarouter_frontend_stats {
    uint64_t sent_frames;
    uint64_t sent_chunks;
    uint64_t received_frames;
    uint64_t received_complete;
    uint64_t received_chunk_acks;
    uint64_t received_chunk_errors;
    uint64_t ack_timeouts;
    uint64_t queue_full;
    uint64_t protocol_errors;
    uint64_t dropped_frames;
    uint64_t pending_acks;
    uint64_t queued_messages;
    uint64_t reader_alive;
    uint64_t reader_errors;
    uint64_t keepalive_ticks;
    uint64_t last_keepalive_ns;
    uint64_t reassembly_expired;
    uint64_t reassembly_rejected;
    uint64_t corrupted_rings;
    uint64_t registration_recoveries;
    uint64_t blocked_wakeups;
    uint64_t dead_process_reaps;
    uint64_t transport_generation;
    uint64_t switch_recoveries;
} datarouter_frontend_stats;

/*
 * All functions below contain Rust panics and translate them to
 * DATAROUTER_ERR_INTERNAL where the return type permits it.
 *
 * A datarouter_frontend handle may be used concurrently by send and snapshot
 * calls. Recv calls must be serialized by the caller so that a short-buffer
 * retry still refers to the same queued message. Close is exclusive: callers
 * must ensure that no operation is in flight and no later operation uses it.
 */

int datarouter_frontend_open(
    const datarouter_frontend_options *options,
    datarouter_frontend **out
);
/* Legacy ABI entry point. It always returns DATAROUTER_ERR_PROTOCOL without
 * reading options; use datarouter_frontend_open_v2. */

uint32_t datarouter_frontend_abi_version(void);

int datarouter_frontend_open_v2(
    const datarouter_frontend_options *options,
    size_t options_size,
    uint32_t abi_version,
    datarouter_frontend **out
);
/* options_size must equal sizeof(datarouter_frontend_options) and abi_version
 * must equal DATAROUTER_FRONTEND_ABI_VERSION. A mismatch returns
 * DATAROUTER_ERR_PROTOCOL before options is dereferenced. */

int datarouter_frontend_send(
    datarouter_frontend *frontend,
    const char *target_service,
    const void *data,
    size_t data_len,
    uint32_t timeout_ms
);

int datarouter_frontend_recv(
    datarouter_frontend *frontend,
    void *out_buf,
    size_t out_capacity,
    datarouter_message_info *out_info,
    uint32_t timeout_ms
);
/*
 * out_buf must reference out_capacity writable bytes (or may be NULL when
 * out_capacity is zero). If the next message does not fit, it remains queued,
 * DATAROUTER_ERR_PAYLOAD_TOO_LARGE is returned, and out_info->len is set to
 * the required capacity. Call again with a larger buffer to receive it.
 */

int datarouter_frontend_registry_snapshot(
    datarouter_frontend *frontend,
    datarouter_registry_snapshot_entry *entries,
    size_t capacity,
    size_t *count
);

int datarouter_frontend_stats_snapshot(
    datarouter_frontend *frontend,
    datarouter_frontend_stats *out_stats
);
/* Legacy ABI entry point. It never writes out_stats and returns
 * DATAROUTER_ERR_PROTOCOL; use datarouter_frontend_stats_snapshot_v2. */

int datarouter_frontend_stats_snapshot_v2(
    datarouter_frontend *frontend,
    datarouter_frontend_stats *out_stats,
    size_t out_size,
    uint32_t abi_version
);
/* out_size must equal sizeof(datarouter_frontend_stats). A size/version
 * mismatch returns DATAROUTER_ERR_PROTOCOL without writing out_stats. */

void datarouter_frontend_close(datarouter_frontend *frontend);

#ifdef __cplusplus
}
#endif

#endif
