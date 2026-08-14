#ifndef DATAROUTER_H
#define DATAROUTER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DATAROUTER_OK 0
#define DATAROUTER_ERR_INVALID_ARGUMENT 1
#define DATAROUTER_ERR_NOT_FOUND 2
#define DATAROUTER_ERR_BUSY 3
#define DATAROUTER_ERR_FULL 4
#define DATAROUTER_ERR_PAYLOAD_TOO_LARGE 5
#define DATAROUTER_ERR_AGAIN 6
#define DATAROUTER_ERR_TIMEOUT 7
#define DATAROUTER_ERR_PERMISSION 8
#define DATAROUTER_ERR_PLATFORM 9
#define DATAROUTER_ERR_PROTOCOL 10
#define DATAROUTER_ERR_INTERNAL 11

#define DATAROUTER_DEFAULT_SHM_NAME "/datarouter"
#define DATAROUTER_MAX_PORTS 64
#define DATAROUTER_DEFAULT_MAX_PORTS 4
#define DATAROUTER_CHANNEL_COUNT (DATAROUTER_DEFAULT_MAX_PORTS * 2)
#ifndef DATAROUTER_CHANNEL_SIZE
#define DATAROUTER_CHANNEL_SIZE 524288U
#endif
/* Official ring geometry: the 512 KiB channel carries a small atomic ring
 * header followed by DATAROUTER_RING_CAPACITY data bytes. A frame adds an
 * 8-byte route/length header, so the largest single-frame payload is
 * DATAROUTER_MAX_APP_PAYLOAD (about 512 KiB - 16). The legacy 60 KiB cap
 * is deliberately gone: the whole ring is usable as one frame. */
#define DATAROUTER_FRAME_HEADER_SIZE 8U
#define DATAROUTER_RING_HEADER_SIZE 8U
#define DATAROUTER_RING_CAPACITY \
    (DATAROUTER_CHANNEL_SIZE - DATAROUTER_RING_HEADER_SIZE)
#define DATAROUTER_MAX_APP_PAYLOAD \
    (DATAROUTER_RING_CAPACITY - DATAROUTER_FRAME_HEADER_SIZE)
#define DATAROUTER_SERVICE_NAME_MAX 31
#define DATAROUTER_SHM_NAME_MAX 120
#define DATAROUTER_DEFAULT_TIMEOUT_MS 3000U

typedef struct datarouter_port datarouter_port;

typedef struct {
    const char *shm_name;
    uint32_t timeout_ms;
    /* Optional shared secret for switch auth. When the switch was started
     * with --token, this must match or datarouter_open fails with
     * DATAROUTER_ERR_PERMISSION. NULL/empty is only accepted when the switch
     * runs with auth disabled. */
    const char *auth_token;
} datarouter_options;

typedef struct {
    char source[DATAROUTER_SERVICE_NAME_MAX + 1U];
    size_t len;
} datarouter_message_info;

typedef int (*datarouter_drain_callback)(
    const datarouter_message_info *info,
    const void *data,
    size_t len,
    void *user_data
);

typedef struct {
    char service_name[DATAROUTER_SERVICE_NAME_MAX + 1U];
    int32_t pid;
    uint32_t route_id;
    uint32_t channel_id;
    uint32_t slot;
    uint64_t last_seen_ns;
} datarouter_registry_snapshot_entry;

typedef struct {
    uint64_t corrupted_rings;
    uint64_t registration_recoveries;
    uint64_t blocked_wakeups;
    uint64_t registration_request_recoveries;
    /* Enabled --reap-dead-processes reclamations. */
    uint64_t dead_process_reaps;
    /* Monotonic identity of the current switch-owned transport mapping. */
    uint64_t transport_generation;
} datarouter_transport_stats;

const char *datarouter_version(void);
const char *datarouter_strerror(int code);

int datarouter_open(const datarouter_options *options, datarouter_port **out);
int datarouter_logon(
    datarouter_port *port,
    const char *service_name,
    char *errbuf,
    size_t errbuf_len
);
int datarouter_send(
    datarouter_port *port,
    const char *target_service,
    const void *data,
    size_t len
);
#ifdef DATAROUTER_UNSAFE
/* 开发中特性：直接数据面。把帧直接写进目标 RX ring，绕过 switch 路由；
 * switch 仅保留控制面。配合正常 datarouter_recv/wait 使用，全链路 2 次拷贝。
 * 限制：同一目标 ring 只允许一个逻辑写者（与 switch 单写者一致）；
 * 目标 ring 满时返回 DATAROUTER_ERR_FULL（无 switch 阻塞队列协议）。 */
int datarouter_unsafe_send(
    datarouter_port *port,
    const char *target_service,
    const void *data,
    size_t len
);
#endif /* DATAROUTER_UNSAFE */
int datarouter_recv(
    datarouter_port *port,
    void *buffer,
    size_t capacity,
    datarouter_message_info *info
);
int datarouter_poll(datarouter_port *port);
int datarouter_frame_len(datarouter_port *port, size_t *payload_len);
int datarouter_touch(datarouter_port *port);
int datarouter_wait(datarouter_port *port, uint32_t timeout_ms);
int datarouter_drain(
    datarouter_port *port,
    uint32_t budget_us,
    size_t max_frames,
    datarouter_drain_callback callback,
    void *user_data,
    size_t *drained
);
int datarouter_registry_snapshot(
    datarouter_port *port,
    datarouter_registry_snapshot_entry *entries,
    size_t capacity,
    size_t *count
);
uint32_t datarouter_ttl_ms(datarouter_port *port);
uint32_t datarouter_max_ports(datarouter_port *port);
uint32_t datarouter_active_ports(datarouter_port *port);
int datarouter_transport_stats_snapshot(
    datarouter_port *port,
    datarouter_transport_stats *out
);
void datarouter_close(datarouter_port *port);

#ifdef __cplusplus
}
#endif

#endif
