#include "datarouter_internal.h"
#include "datarouter_registry.h"
#include "datarouter_scheduler.h"
#include "datarouter_signal.h"

#include <errno.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sched.h>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

const char *datarouter_version(void) {
    return DATAROUTER_VERSION;
}

uint64_t datarouter_auth_fingerprint(const char *token) {
    /* FNV-1a 64. The fingerprint is not reversible; the raw token is never
     * stored in shared memory. Zero is reserved for "auth disabled". */
    if (!token || token[0] == '\0') {
        return 0ULL;
    }
    uint64_t hash = 14695981039346656037ULL;
    for (const unsigned char *cursor = (const unsigned char *)token; *cursor; cursor++) {
        hash ^= (uint64_t)*cursor;
        hash *= 1099511628211ULL;
    }
    return hash == 0ULL ? 1ULL : hash;
}

const char *datarouter_strerror(int code) {
    switch (code) {
        case DATAROUTER_OK:
            return "ok";
        case DATAROUTER_ERR_INVALID_ARGUMENT:
            return "invalid argument";
        case DATAROUTER_ERR_NOT_FOUND:
            return "not found";
        case DATAROUTER_ERR_BUSY:
            return "busy";
        case DATAROUTER_ERR_FULL:
            return "full";
        case DATAROUTER_ERR_PAYLOAD_TOO_LARGE:
            return "payload too large";
        case DATAROUTER_ERR_AGAIN:
            return "try again";
        case DATAROUTER_ERR_TIMEOUT:
            return "timeout";
        case DATAROUTER_ERR_PERMISSION:
            return "permission denied";
        case DATAROUTER_ERR_PLATFORM:
            return "platform error";
        case DATAROUTER_ERR_PROTOCOL:
            return "protocol error";
        case DATAROUTER_ERR_INTERNAL:
            return "internal error";
        default:
            return "unknown error";
    }
}

uint64_t datarouter_now_ns(void) {
#ifdef _WIN32
    LARGE_INTEGER frequency;
    LARGE_INTEGER counter;
    if (!QueryPerformanceFrequency(&frequency) || !QueryPerformanceCounter(&counter)) {
        return 0;
    }
    return (uint64_t)((counter.QuadPart * 1000000000ULL) / (uint64_t)frequency.QuadPart);
#else
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return ((uint64_t)ts.tv_sec * 1000000000ULL) + (uint64_t)ts.tv_nsec;
#endif
}

void datarouter_sleep_us(uint32_t usec) {
#ifdef _WIN32
    DWORD ms = (DWORD)((usec + 999U) / 1000U);
    Sleep(ms);
#else
    struct timespec ts;
    ts.tv_sec = (time_t)(usec / 1000000U);
    ts.tv_nsec = (long)(usec % 1000000U) * 1000L;
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {
    }
#endif
}

void datarouter_yield(void) {
#ifdef _WIN32
    Sleep(0);
#else
    sched_yield();
#endif
}

int datarouter_pid_alive(pid_t pid) {
    if (pid <= 0) {
        return 0;
    }
#ifdef _WIN32
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, (DWORD)pid);
    if (!process) {
        return GetLastError() == ERROR_ACCESS_DENIED;
    }
    DWORD exit_code = 0;
    int alive = GetExitCodeProcess(process, &exit_code) && exit_code == STILL_ACTIVE;
    CloseHandle(process);
    return alive;
#else
    if (kill(pid, 0) == 0) {
        return 1;
    }
    return errno == EPERM;
#endif
}

int datarouter_process_identity(pid_t pid, uint32_t *uid, uint64_t *start_id) {
    if (pid <= 0 || !uid || !start_id) {
        return DATAROUTER_ERR_INVALID_ARGUMENT;
    }
#ifdef _WIN32
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, (DWORD)pid);
    if (!process) {
        return GetLastError() == ERROR_ACCESS_DENIED
            ? DATAROUTER_ERR_PERMISSION
            : DATAROUTER_ERR_NOT_FOUND;
    }
    FILETIME created;
    FILETIME exited;
    FILETIME kernel;
    FILETIME user;
    if (!GetProcessTimes(process, &created, &exited, &kernel, &user)) {
        CloseHandle(process);
        return DATAROUTER_ERR_PLATFORM;
    }
    DWORD exit_code = 0U;
    if (!GetExitCodeProcess(process, &exit_code) || exit_code != STILL_ACTIVE) {
        CloseHandle(process);
        return DATAROUTER_ERR_NOT_FOUND;
    }
    CloseHandle(process);
    ULARGE_INTEGER value;
    value.LowPart = created.dwLowDateTime;
    value.HighPart = created.dwHighDateTime;
    *uid = 0U;
    *start_id = value.QuadPart;
    return DATAROUTER_OK;
#else
    char proc_path[64];
    int written = snprintf(proc_path, sizeof(proc_path), "/proc/%ld", (long)pid);
    if (written <= 0 || (size_t)written >= sizeof(proc_path)) {
        return DATAROUTER_ERR_INVALID_ARGUMENT;
    }
    struct stat st;
    if (stat(proc_path, &st) != 0) {
        if (errno == ENOENT || errno == ESRCH) {
            return DATAROUTER_ERR_NOT_FOUND;
        }
        return errno == EACCES ? DATAROUTER_ERR_PERMISSION : DATAROUTER_ERR_PLATFORM;
    }

    written = snprintf(proc_path, sizeof(proc_path), "/proc/%ld/stat", (long)pid);
    if (written <= 0 || (size_t)written >= sizeof(proc_path)) {
        return DATAROUTER_ERR_INVALID_ARGUMENT;
    }
    FILE *file = fopen(proc_path, "r");
    if (!file) {
        return errno == ENOENT ? DATAROUTER_ERR_NOT_FOUND : DATAROUTER_ERR_PLATFORM;
    }
    char line[4096];
    if (!fgets(line, sizeof(line), file)) {
        fclose(file);
        return DATAROUTER_ERR_PLATFORM;
    }
    fclose(file);

    /* /proc/<pid>/stat field 2 is parenthesized and may contain spaces. */
    char *after_name = strrchr(line, ')');
    if (!after_name || after_name[1] != ' ') {
        return DATAROUTER_ERR_PROTOCOL;
    }
    char *cursor = after_name + 2;
    char *save = NULL;
    char *token = strtok_r(cursor, " ", &save);
    if (!token || token[1] != '\0' || token[0] == 'Z' || token[0] == 'X' || token[0] == 'x') {
        return token ? DATAROUTER_ERR_NOT_FOUND : DATAROUTER_ERR_PROTOCOL;
    }
    for (unsigned int field = 3U; token && field < 22U; field++) {
        token = strtok_r(NULL, " ", &save);
    }
    if (!token) {
        return DATAROUTER_ERR_PROTOCOL;
    }
    errno = 0;
    char *end = NULL;
    unsigned long long parsed = strtoull(token, &end, 10);
    if (errno != 0 || end == token || (*end != '\0' && *end != '\n') || parsed == 0ULL) {
        return DATAROUTER_ERR_PROTOCOL;
    }
    *uid = (uint32_t)st.st_uid;
    *start_id = (uint64_t)parsed;
    return DATAROUTER_OK;
#endif
}

int datarouter_process_identity_matches(pid_t pid, uint32_t uid, uint64_t start_id) {
    uint32_t actual_uid = 0U;
    uint64_t actual_start_id = 0ULL;
    return start_id != 0ULL &&
           datarouter_process_identity(pid, &actual_uid, &actual_start_id) == DATAROUTER_OK &&
           actual_uid == uid && actual_start_id == start_id;
}

int datarouter_valid_service_name(const char *service_name) {
    if (!service_name || service_name[0] == '\0') {
        return 0;
    }
    size_t len = 0U;
    while (len <= DATAROUTER_SERVICE_NAME_MAX && service_name[len] != '\0') {
        len++;
    }
    if (len == 0U || len > DATAROUTER_SERVICE_NAME_MAX) {
        return 0;
    }
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)service_name[i];
        if (!((c >= 'A' && c <= 'Z') ||
              (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') ||
              c == '_' || c == '-' || c == '.')) {
            return 0;
        }
    }
    return 1;
}

int datarouter_valid_shm_name(const char *name) {
    if (!name || name[0] != '/' || name[1] == '\0') {
        return 0;
    }
    size_t len = 1U;
    while (len <= DATAROUTER_SHM_NAME_MAX && name[len] != '\0') {
        unsigned char c = (unsigned char)name[len];
        if (!((c >= 'A' && c <= 'Z') ||
              (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') ||
              c == '_' || c == '-' || c == '.')) {
            return 0;
        }
        len++;
    }
    return len > 1U && len <= DATAROUTER_SHM_NAME_MAX && name[len] == '\0';
}

size_t datarouter_shared_memory_size(uint32_t max_ports) {
    if (max_ports == 0U || max_ports > DATAROUTER_MAX_PORTS) {
        return 0U;
    }
    size_t channel_count = (size_t)max_ports * 2U;
    if (channel_count > (SIZE_MAX - sizeof(datarouter_control_plane)) / sizeof(datarouter_channel)) {
        return 0U;
    }
    return sizeof(datarouter_control_plane) + channel_count * sizeof(datarouter_channel);
}

uint32_t datarouter_active_max_ports(const datarouter_shared_memory *shm) {
    if (!shm || shm->control.max_ports == 0U || shm->control.max_ports > DATAROUTER_MAX_PORTS) {
        return 0U;
    }
    return shm->control.max_ports;
}

uint32_t datarouter_route_for_slot(uint32_t slot_index) {
    return datarouter_route_for_slot_generation(slot_index, 1U);
}

uint32_t datarouter_route_for_slot_generation(uint32_t slot_index, uint32_t generation) {
    if (slot_index >= DATAROUTER_MAX_PORTS) {
        return 0U;
    }
    uint32_t normalized_generation = generation & DATAROUTER_ROUTE_GENERATION_MASK;
    if (normalized_generation == 0U) {
        normalized_generation = 1U;
    }
    return (normalized_generation << DATAROUTER_ROUTE_SLOT_BITS) |
           (slot_index & DATAROUTER_ROUTE_SLOT_MASK);
}

void datarouter_touch_entry(datarouter_registry_entry *entry, uint64_t now_ns) {
    if (entry) {
        atomic_store_explicit(&entry->last_active_ns, now_ns, memory_order_release);
    }
}

int datarouter_try_lock(atomic_uint *lock) {
    unsigned int expected = DATAROUTER_CONTROL_LOCK_FREE;
    return atomic_compare_exchange_strong_explicit(
        lock,
        &expected,
        DATAROUTER_CONTROL_LOCK_HELD,
        memory_order_acquire,
        memory_order_relaxed
    );
}

void datarouter_unlock(atomic_uint *lock) {
    atomic_store_explicit(lock, DATAROUTER_CONTROL_LOCK_FREE, memory_order_release);
}

void datarouter_set_error(char *errbuf, size_t errbuf_len, const char *message) {
    if (!errbuf || errbuf_len == 0U) {
        return;
    }
    if (!message) {
        message = "";
    }
    snprintf(errbuf, errbuf_len, "%s", message);
}

int datarouter_init_shared_memory_with_ttl(
    datarouter_shared_memory *shm,
    uint32_t max_ports,
    uint32_t ttl_ms
) {
    return datarouter_init_shared_memory_full(shm, max_ports, ttl_ms, 0ULL);
}

int datarouter_init_shared_memory_full(
    datarouter_shared_memory *shm,
    uint32_t max_ports,
    uint32_t ttl_ms,
    uint64_t auth_fingerprint
) {
    size_t total_size = datarouter_shared_memory_size(max_ports);
    if (!shm || ttl_ms == 0U || total_size == 0U || total_size > UINT32_MAX) {
        return DATAROUTER_ERR_INVALID_ARGUMENT;
    }
    memset(shm, 0, total_size);
    shm->control.magic = DATAROUTER_MAGIC;
    shm->control.layout_version = DATAROUTER_LAYOUT_VERSION;
    shm->control.max_ports = max_ports;
    shm->control.channel_count = max_ports * 2U;
    shm->control.channel_size = DATAROUTER_CHANNEL_SIZE;
    shm->control.control_size = (uint32_t)sizeof(datarouter_control_plane);
    shm->control.data_offset = (uint32_t)sizeof(datarouter_control_plane);
    shm->control.total_size = (uint32_t)total_size;
    shm->control.auth_fingerprint = auth_fingerprint;
    uint32_t switch_uid = 0U;
    uint64_t switch_start_id = 0ULL;
#ifdef _WIN32
    int32_t switch_pid = (int32_t)GetCurrentProcessId();
#else
    int32_t switch_pid = (int32_t)getpid();
#endif
    if (datarouter_process_identity((pid_t)switch_pid, &switch_uid, &switch_start_id) != DATAROUTER_OK) {
        return DATAROUTER_ERR_PLATFORM;
    }
    shm->control.switch_pid = switch_pid;
    shm->control.switch_uid = switch_uid;
    shm->control.switch_start_id = switch_start_id;
    shm->control.ttl_ms = ttl_ms;
    atomic_store_explicit(&shm->control.control_lock, DATAROUTER_CONTROL_LOCK_FREE, memory_order_release);
    atomic_store_explicit(&shm->control.request_ready_mask, 0ULL, memory_order_release);
    datarouter_scheduler_clear(shm);
    atomic_store_explicit(&shm->control.corrupt_slot_mask, 0ULL, memory_order_release);
    atomic_store_explicit(&shm->control.corrupted_rings, 0ULL, memory_order_release);
    atomic_store_explicit(&shm->control.registration_recoveries, 0ULL, memory_order_release);
    atomic_store_explicit(&shm->control.blocked_wakeups, 0ULL, memory_order_release);
    atomic_store_explicit(&shm->control.registration_request_recoveries, 0ULL, memory_order_release);
    atomic_store_explicit(&shm->control.dead_process_reaps, 0ULL, memory_order_release);
    /* Every freshly-created switch mapping is a distinct transport generation.
     * Frontends expose this alongside their local recovery counter so callers
     * can treat a switch replacement as an epoch boundary. */
    atomic_store_explicit(
        &shm->control.transport_generation,
        switch_start_id,
        memory_order_release
    );
    for (uint32_t source = 0U; source < DATAROUTER_MAX_PORTS; source++) {
        for (uint32_t target = 0U; target < DATAROUTER_MAX_PORTS; target++) {
            atomic_store_explicit(
                &shm->control.inflight_frames[source][target],
                0U,
                memory_order_relaxed
            );
        }
    }
    for (uint32_t i = 0U; i < DATAROUTER_REGISTRATION_REQUEST_SLOTS; i++) {
        datarouter_register_request *request = &shm->control.register_requests[i];
        request->operation = DATAROUTER_REG_OP_NONE;
        atomic_store_explicit(&request->claim_token, 0ULL, memory_order_release);
        atomic_store_explicit(&request->state, DATAROUTER_REG_EMPTY, memory_order_release);
    }
    if (datarouter_signal_init(&shm->control.switch_signal) != DATAROUTER_OK) {
        return DATAROUTER_ERR_PLATFORM;
    }
    if (datarouter_signal_init(&shm->control.registration_signal) != DATAROUTER_OK) {
        return DATAROUTER_ERR_PLATFORM;
    }

    for (uint32_t i = 0; i < max_ports; i++) {
        if (datarouter_signal_init(&shm->control.rx_signal[i]) != DATAROUTER_OK) {
            return DATAROUTER_ERR_PLATFORM;
        }
        shm->control.slots[i].slot_index = i;
        shm->control.slots[i].rx_channel_index = DATAROUTER_RX_CHANNEL(i);
        shm->control.slots[i].tx_channel_index = DATAROUTER_TX_CHANNEL(i);
        datarouter_registry_clear_entry(&shm->control.registry[i], i);
    }
    datarouter_channel *channels = (datarouter_channel *)((uint8_t *)shm + shm->control.data_offset);
    for (uint32_t i = 0; i < shm->control.channel_count; i++) {
        datarouter_ring_reset(&channels[i]);
    }
    atomic_store_explicit(&shm->control.initialized, 1U, memory_order_release);
    return DATAROUTER_OK;
}

int datarouter_init_shared_memory(datarouter_shared_memory *shm, uint32_t max_ports) {
    return datarouter_init_shared_memory_with_ttl(shm, max_ports, DATAROUTER_DEFAULT_TTL_MS);
}

int datarouter_validate_shared_memory(const datarouter_shared_memory *shm, size_t map_size) {
    if (!shm) {
        return DATAROUTER_ERR_INVALID_ARGUMENT;
    }
    if (map_size < sizeof(datarouter_control_plane)) {
        return DATAROUTER_ERR_PROTOCOL;
    }
    if (atomic_load_explicit(&shm->control.initialized, memory_order_acquire) != 1U) {
        return DATAROUTER_ERR_AGAIN;
    }
    if (shm->control.magic != DATAROUTER_MAGIC ||
        shm->control.layout_version != DATAROUTER_LAYOUT_VERSION ||
        shm->control.max_ports == 0U ||
        shm->control.max_ports > DATAROUTER_MAX_PORTS ||
        shm->control.channel_count != shm->control.max_ports * 2U ||
        shm->control.channel_size != DATAROUTER_CHANNEL_SIZE ||
        shm->control.control_size != (uint32_t)sizeof(datarouter_control_plane) ||
        shm->control.data_offset != (uint32_t)sizeof(datarouter_control_plane) ||
        shm->control.total_size != (uint32_t)datarouter_shared_memory_size(shm->control.max_ports) ||
        map_size < shm->control.total_size) {
        return DATAROUTER_ERR_PROTOCOL;
    }
    if (shm->control.switch_pid <= 0 || shm->control.switch_start_id == 0ULL ||
        shm->control.ttl_ms == 0U) {
        return DATAROUTER_ERR_PROTOCOL;
    }
    return DATAROUTER_OK;
}

datarouter_channel *datarouter_rx_channel(datarouter_shared_memory *shm, uint32_t slot_index) {
    uint32_t max_ports = datarouter_active_max_ports(shm);
    if (!shm || slot_index >= max_ports) {
        return NULL;
    }
    datarouter_channel *channels = (datarouter_channel *)((uint8_t *)shm + shm->control.data_offset);
    return &channels[DATAROUTER_RX_CHANNEL(slot_index)];
}

datarouter_channel *datarouter_tx_channel(datarouter_shared_memory *shm, uint32_t slot_index) {
    uint32_t max_ports = datarouter_active_max_ports(shm);
    if (!shm || slot_index >= max_ports) {
        return NULL;
    }
    datarouter_channel *channels = (datarouter_channel *)((uint8_t *)shm + shm->control.data_offset);
    return &channels[DATAROUTER_TX_CHANNEL(slot_index)];
}
