#ifndef DATAROUTER_INTERNAL_H
#define DATAROUTER_INTERNAL_H

#include "../include/datarouter.h"

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#ifdef _MSC_VER
typedef int mode_t;
typedef int pid_t;
#else
#include <sys/types.h>
#endif
typedef atomic_uint datarouter_signal;
typedef CRITICAL_SECTION datarouter_mutex;
#else
#include <pthread.h>
#include <semaphore.h>
#include <sys/types.h>
typedef sem_t datarouter_signal;
typedef pthread_mutex_t datarouter_mutex;
#endif

#define DATAROUTER_VERSION "0.2.0"
#define DATAROUTER_MAGIC 0x534D4342U
#define DATAROUTER_LAYOUT_VERSION 13U
#define DATAROUTER_KEEP_MAGIC 0x4B454550u /* "KEEP" */
#define DATAROUTER_KEEP_LAYOUT_VERSION 2u
#define DATAROUTER_KEEP_HEADER_SIZE 128u
#define DATAROUTER_KEEP_SLOT_SIZE 32u
#define DATAROUTER_KEEP_SLOTS_PER_ENTRY 2u /* 开放寻址负载 <50% */
#define DATAROUTER_KEEP_EVICT_PER_PUT 64u
#define DATAROUTER_KEEP_PREFIX_SCAN_LIMIT 4096u
#define DATAROUTER_KEEP_LOCK_SPIN_LIMIT 50000u
#define DATAROUTER_KEEP_SEQLOCK_RETRY 64u
#define DATAROUTER_KEEP_MIN_SIZE 4096u
#define KEEP_EMPTY 0xFFFFFFFFu
#define KEEP_TOMBSTONE 0xFFFFFFFEu
#define KEEP_WRITE_LOCK_FREE 0U
#define KEEP_WRITE_LOCK_HELD 1U
#define DATAROUTER_ROUTE_SLOT_BITS 6U
#define DATAROUTER_ROUTE_SLOT_MASK ((1U << DATAROUTER_ROUTE_SLOT_BITS) - 1U)
#define DATAROUTER_ROUTE_GENERATION_MASK 0x03ffffffU
#define DATAROUTER_RX_CHANNEL(slot_index) ((uint32_t)((slot_index) * 2U))
#define DATAROUTER_TX_CHANNEL(slot_index) ((uint32_t)((slot_index) * 2U + 1U))
/* Ring geometry is defined publicly in include/datarouter.h. The assert below
 * (after the ring header typedef) keeps the internal header in lockstep. */
#define DATAROUTER_CONTROL_LOCK_FREE 0U
#define DATAROUTER_CONTROL_LOCK_HELD 1U
#define DATAROUTER_DEFAULT_TTL_MS 30000U
#define DATAROUTER_SWITCH_IDLE_US 1000U
#define DATAROUTER_PORT_SPIN_BUDGET_US 50U
#define DATAROUTER_PORT_DRAIN_BUDGET_US 200U
#define DATAROUTER_PORT_DRAIN_MAX_BUDGET_US 1000U
#define DATAROUTER_SWITCH_ROUTE_BUDGET_US 200U
#define DATAROUTER_SWITCH_ROUTE_MAX_FRAMES 1024U
#define DATAROUTER_REGISTRATION_REQUEST_SLOTS 64U
#define DATAROUTER_SWITCH_IDENTITY_CACHE_NS 10000000ULL
/* Upper bound a slot release waits for an in-flight direct writer to finish
 * (write-pin drain). A direct write is a bounded memcpy (microseconds); this
 * budget only binds the rare case where the target owner dies mid-write. */
#define DATAROUTER_DIRECT_WRITER_WAIT_NS 500000ULL
#ifndef DATAROUTER_SWITCH_SPIN_BUDGET_US
#define DATAROUTER_SWITCH_SPIN_BUDGET_US 200U
#endif
#ifndef DATAROUTER_SWITCH_WAIT_MS
#define DATAROUTER_SWITCH_WAIT_MS 1U
#endif

typedef enum {
    DATAROUTER_PORT_EMPTY = 0,
    DATAROUTER_PORT_ACTIVE = 1,
    DATAROUTER_PORT_RELEASING = 2,
    DATAROUTER_PORT_DRAINING = 3
} datarouter_port_state;

typedef enum {
    DATAROUTER_REG_EMPTY = 0,
    DATAROUTER_REG_CLAIMED = 1,
    DATAROUTER_REG_PENDING = 2,
    DATAROUTER_REG_PROCESSING = 3,
    DATAROUTER_REG_ACCEPTED = 4,
    DATAROUTER_REG_REJECTED = 5,
    DATAROUTER_REG_CONSUMING = 6,
    DATAROUTER_REG_ABANDONED = 7,
    DATAROUTER_REG_RECOVERING = 8
} datarouter_reg_state;

typedef enum {
    DATAROUTER_REG_OP_NONE = 0,
    DATAROUTER_REG_OP_REGISTER = 1,
    DATAROUTER_REG_OP_UNREGISTER = 2,
    DATAROUTER_REG_OP_KEEP_GRANT = 3
} datarouter_reg_operation;

typedef struct {
    atomic_uint head;
    atomic_uint tail;
} datarouter_ring_header;

_Static_assert(sizeof(datarouter_ring_header) == DATAROUTER_RING_HEADER_SIZE, "ring header size mismatch");
/* The ring index wrap is a mask, so the data capacity must be a power of two.
 * Default DATAROUTER_CHANNEL_SIZE=524288 (2^19) satisfies this. */
_Static_assert(
    (DATAROUTER_RING_CAPACITY & (DATAROUTER_RING_CAPACITY - 1U)) == 0U,
    "DATAROUTER_RING_CAPACITY must be a power of two for masked indexing"
);

typedef struct {
    datarouter_ring_header header;
    uint8_t data[DATAROUTER_RING_CAPACITY];
} datarouter_channel;

typedef struct {
    atomic_uint state;
    uint32_t slot_index;
    atomic_uint route_id;
    uint32_t generation;
    atomic_int pid;
    atomic_uint uid;
    atomic_ullong process_start_id;
    /* Owner-thread pin counter (recv/touch/drain from the slot owner). The
     * switch's finalize_release handshake zeroes it only when the owner is
     * dead, because a dead owner's threads can never unpin. */
    atomic_uint io_users;
    /* Direct (unsafe) data-plane writer pin counter + last-writer identity.
     * Kept separate from io_users so a live foreign direct writer is never
     * mistaken for a stale owner pin: finalize_release drains this counter
     * while the writer is alive, and only force-zeroes it when the recorded
     * writer process is dead (crashed mid-write). The one-logical-writer
     * constraint makes the single identity triplet sufficient. */
    atomic_uint direct_writers;
    atomic_int direct_writer_pid;
    atomic_uint direct_writer_uid;
    atomic_ullong direct_writer_start_id;
    atomic_ullong last_active_ns;
    atomic_uchar service_name[DATAROUTER_SERVICE_NAME_MAX + 1U];
} datarouter_registry_entry;

typedef struct {
    uint32_t magic;
    uint32_t layout_version;
    uint32_t capacity;        /* arena 有效字节数 */
    uint32_t slot_capacity;   /* 哈希槽数 */
    uint32_t entry_count;     /* 存活条目 */
    uint32_t used;            /* 当前占用 arena 字节数 */
    uint64_t hits;
    uint64_t misses;
    uint64_t expired;
    uint64_t evictions;
    _Atomic uint64_t generation;   /* seqlock：写开始置奇、写结束置偶、单调递增 */
    atomic_uint writer_lock;      /* 写方串行化（CAS spin）+ 崩溃自愈 */
    int32_t writer_pid;           /* 持锁写方 pid；0 = 无写方 */
    atomic_uint recovery_seq;     /* 自愈后递增的幂等恢复计数 */
    uint32_t write_head;          /* arena 环形写锚 */
    uint32_t evict_head;          /* arena 环形淘汰锚 */
    uint32_t tombstones;          /* 墓碑数（触发 rehash 压实） */
    uint32_t _reserved[10];       /* 补足到 128B（% 64 == 0，8 对齐） */
} datarouter_keep_header;
_Static_assert(sizeof(datarouter_keep_header) == 128u && sizeof(datarouter_keep_header) % 64u == 0u,
               "datarouter_keep_header ABI mismatch");

typedef struct {
    uint64_t key_hash;      /* FNV-1a 64（槽位定位 + 删除时反向位移判定） */
    uint64_t expires_ns;    /* 绝对纳秒；0 = 永不过期 */
    uint32_t key_offset;    /* 指向 arena 条目区；-1U = 空槽（无墓碑） */
    uint32_t value_offset;  /* 值在条目内的偏移（相对 arena_base） */
    uint32_t value_len;
    uint32_t age;
} datarouter_keep_slot;
_Static_assert(sizeof(datarouter_keep_slot) == 32u, "datarouter_keep_slot ABI mismatch");

/* Arena 条目头（环形分配：key/value 前 16 字节头，4 字节对齐）。 */
typedef struct {
    uint32_t key_len;
    uint32_t value_len;
    uint64_t expires_ns;
} datarouter_keep_entry_hdr;
_Static_assert(sizeof(datarouter_keep_entry_hdr) == 16u, "datarouter_keep_entry_hdr size");

typedef struct {
    atomic_uint state;
    atomic_ullong claim_token;
    uint32_t operation;
    int32_t owner_pid;
    uint32_t owner_uid;
    uint64_t owner_start_id;
    uint64_t request_nonce;
    uint64_t response_nonce;
    uint64_t deadline_ns;
    char service_name[DATAROUTER_SERVICE_NAME_MAX + 1U];
    uint32_t route_id;
    uint32_t assigned_slot;
    uint32_t assigned_route_id;
    int32_t result_code;
    char message[96];
} datarouter_register_request;

typedef struct {
    uint32_t slot_index;
    uint32_t rx_channel_index;
    uint32_t tx_channel_index;
} datarouter_slot_descriptor;

typedef struct {
    uint32_t magic;
    uint32_t layout_version;
    uint32_t max_ports;
    uint32_t channel_count;
    uint32_t channel_size;
    uint32_t control_size;
    uint32_t data_offset;
    uint32_t total_size;
    int32_t switch_pid;
    uint32_t switch_uid;
    uint64_t switch_start_id;
    uint32_t ttl_ms;
    uint64_t auth_fingerprint;
    atomic_uint initialized;
    atomic_uint control_lock;
    atomic_ullong request_ready_mask;
    atomic_ullong tx_ready_mask;
    atomic_ullong tx_blocked_mask;
    atomic_ullong corrupt_slot_mask;
    atomic_ullong corrupted_rings;
    atomic_ullong registration_recoveries;
    atomic_ullong blocked_wakeups;
    atomic_ullong registration_request_recoveries;
    atomic_ullong dead_process_reaps;
    atomic_ullong transport_generation;
    /* KEEP 区域（layout 13）：keep_offset 指向 keep 头部（位于 channel blob 之后），
     * keep_size==0 表示关闭。keep_write_allow_bitmap 位图仅由 Switch 在 control_lock
     * 临界区内修改。 */
    uint32_t keep_offset;
    uint32_t keep_size;
    atomic_ullong keep_write_allow_bitmap;
    atomic_uint scheduler_cursor;
    atomic_uint inflight_frames[DATAROUTER_MAX_PORTS][DATAROUTER_MAX_PORTS];
    datarouter_register_request register_requests[DATAROUTER_REGISTRATION_REQUEST_SLOTS];
    datarouter_signal switch_signal;
    datarouter_signal registration_signal;
    datarouter_signal rx_signal[DATAROUTER_MAX_PORTS];
    datarouter_registry_entry registry[DATAROUTER_MAX_PORTS];
    datarouter_slot_descriptor slots[DATAROUTER_MAX_PORTS];
} datarouter_control_plane;

typedef struct {
    datarouter_control_plane control;
} datarouter_shared_memory;

struct datarouter_port {
    int fd;
    size_t map_size;
    datarouter_shared_memory *shm;
    void *platform_handle;
    uint32_t slot_index;
    uint32_t route_id;
    char service_name[DATAROUTER_SERVICE_NAME_MAX + 1U];
    uint32_t cached_source_route;
    char cached_source_name[DATAROUTER_SERVICE_NAME_MAX + 1U];
    uint32_t timeout_ms;
    uint64_t process_start_id;
    int32_t switch_pid;
    uint32_t switch_uid;
    uint64_t switch_start_id;
    atomic_ullong last_switch_identity_check_ns;
    atomic_uint switch_dead;
    datarouter_mutex tx_mutex;
    /* Serializes concurrent ring reads in the direct (unsafe) data plane,
     * where multiple application threads may call datarouter_recv on the
     * same SPSC RX ring without a reader thread. Process-local. */
    datarouter_mutex rx_mutex;
    uint8_t *drain_buffer;
    size_t drain_buffer_capacity;
};

typedef struct {
    int fd;
    size_t map_size;
    datarouter_shared_memory *shm;
    int owner;
    void *platform_handle;
} datarouter_mapping;

int datarouter_platform_create(
    const char *name,
    mode_t mode,
    size_t size,
    datarouter_mapping *out
);
int datarouter_platform_open(const char *name, size_t size_hint, datarouter_mapping *out);
int datarouter_platform_unlink(const char *name);
void datarouter_platform_close(datarouter_mapping *mapping);

uint64_t datarouter_now_ns(void);
void datarouter_sleep_us(uint32_t usec);
void datarouter_yield(void);
int datarouter_pid_alive(pid_t pid);
int datarouter_process_identity(pid_t pid, uint32_t *uid, uint64_t *start_id);
int datarouter_process_identity_matches(pid_t pid, uint32_t uid, uint64_t start_id);

int datarouter_valid_service_name(const char *service_name);
int datarouter_valid_shm_name(const char *name);
int datarouter_init_shared_memory(datarouter_shared_memory *shm, uint32_t max_ports);
int datarouter_init_shared_memory_with_ttl(
    datarouter_shared_memory *shm,
    uint32_t max_ports,
    uint32_t ttl_ms
);
int datarouter_init_shared_memory_full(
    datarouter_shared_memory *shm,
    uint32_t max_ports,
    uint32_t ttl_ms,
    uint64_t auth_fingerprint
);
int datarouter_init_shared_memory_with_keep(
    datarouter_shared_memory *shm,
    uint32_t max_ports,
    uint32_t ttl_ms,
    uint64_t auth_fingerprint,
    uint32_t keep_size,
    uint32_t keep_slot_capacity
);
uint64_t datarouter_auth_fingerprint(const char *token);
int datarouter_validate_shared_memory(const datarouter_shared_memory *shm, size_t map_size);
size_t datarouter_shared_memory_size(uint32_t max_ports);
size_t datarouter_shared_memory_size_with_keep(uint32_t max_ports, uint32_t keep_size);
/* Process-global data-plane copy strategy ("scalar" or "avx2"), selected once
 * from DATAROUTER_COPY=scalar|avx2 (default scalar = libc memcpy; avx2 is a
 * benchmark-only hand-rolled 256-bit loop that measured slower). */
const char *datarouter_copy_strategy(void);
uint32_t datarouter_active_max_ports(const datarouter_shared_memory *shm);

void datarouter_ring_reset(datarouter_channel *channel);
size_t datarouter_ring_used(const datarouter_channel *channel);
size_t datarouter_ring_free(const datarouter_channel *channel);
int datarouter_ring_validate(const datarouter_channel *channel, size_t *used);
int datarouter_ring_write(datarouter_channel *channel, const uint8_t *data, size_t len);
int datarouter_ring_write_parts(
    datarouter_channel *channel,
    const uint8_t *first,
    size_t first_len,
    const uint8_t *second,
    size_t second_len
);
int datarouter_ring_peek(const datarouter_channel *channel, uint8_t *data, size_t len);
int datarouter_ring_read(datarouter_channel *channel, uint8_t *data, size_t len);
int datarouter_ring_skip(datarouter_channel *channel, size_t len);
int datarouter_ring_transfer_frame(
    datarouter_channel *source,
    datarouter_channel *target,
    const uint8_t *header,
    size_t header_len,
    size_t source_frame_len
);

datarouter_channel *datarouter_rx_channel(datarouter_shared_memory *shm, uint32_t slot_index);
datarouter_channel *datarouter_tx_channel(datarouter_shared_memory *shm, uint32_t slot_index);
uint32_t datarouter_route_for_slot(uint32_t slot_index);
uint32_t datarouter_route_for_slot_generation(uint32_t slot_index, uint32_t generation);
void datarouter_touch_entry(datarouter_registry_entry *entry, uint64_t now_ns);
int datarouter_try_lock(atomic_uint *lock);
void datarouter_unlock(atomic_uint *lock);
void datarouter_set_error(char *errbuf, size_t errbuf_len, const char *message);

/* ---- KEEP：核心结构（src/keep.c），Rust 只经 FFI 委托 ---- */
void datarouter_keep_init(datarouter_shared_memory *shm, uint32_t slot_capacity);
datarouter_keep_header *datarouter_keep_header_of(datarouter_shared_memory *shm);
int datarouter_keep_get(
    datarouter_port *port,
    const void *key, size_t key_len,
    void *out, size_t out_cap, size_t *out_len);
int datarouter_keep_get_ref(
    datarouter_port *port,
    const void *key, size_t key_len,
    const void **value_out, size_t *value_len_out,
    uint64_t *seq_out);
int datarouter_keep_verify_ref(datarouter_port *port, uint64_t seq);
int datarouter_keep_put(
    datarouter_port *port,
    const void *key, size_t key_len,
    const void *value, size_t value_len,
    uint64_t ttl_ns);
int datarouter_keep_invalidate(datarouter_port *port, const void *key, size_t key_len);
int datarouter_keep_invalidate_prefix(
    datarouter_port *port, const void *prefix, size_t prefix_len);
int datarouter_keep_stats(datarouter_port *port, datarouter_keep_stats_t *out);
/* 控制面保留位：datarouter_keep_grant 公开声明见 include/datarouter.h，实现在 port.c。 */

#endif
