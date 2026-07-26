#include "config.h"
#include "datarouter_registry.h"
#include "datarouter_router.h"
#include "datarouter_scheduler.h"
#include "datarouter_signal.h"

#include <errno.h>
#include <inttypes.h>
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <process.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#define DATAROUTER_CREATE_RETRIES 4U

typedef enum {
    SWITCH_LOG_ERROR = 0,
    SWITCH_LOG_WARN = 1,
    SWITCH_LOG_INFO = 2,
    SWITCH_LOG_DEBUG = 3,
    SWITCH_LOG_TRACE = 4
} switch_log_level;

typedef struct {
    const char *shm_name;
    mode_t mode;
    uint32_t ttl_ms;
    char allowlist[DATAROUTER_MAX_PORTS][DATAROUTER_SERVICE_NAME_MAX + 1U];
    uint32_t allowlist_len;
    int allow_any;
    const char *config_path;
    int config_mode;
    datarouter_config config;
    int once;
    int reap_dead_processes;
    switch_log_level log_level;
} switch_options;

typedef struct {
#ifdef _WIN32
    int unused;
#else
    int fd;
#endif
} switch_start_lock;

static volatile sig_atomic_t g_stop = 0;

static int32_t switch_pid(void) {
#ifdef _WIN32
    return (int32_t)_getpid();
#else
    return (int32_t)getpid();
#endif
}

static int acquire_start_lock(const char *shm_name, switch_start_lock *lock) {
    if (!datarouter_valid_shm_name(shm_name) || !lock) {
        return DATAROUTER_ERR_INVALID_ARGUMENT;
    }
#ifdef _WIN32
    lock->unused = 0;
    return DATAROUTER_OK;
#else
    lock->fd = -1;
    char path[256];
    int written = snprintf(
        path,
        sizeof(path),
        "/tmp/datarouter-switch-%lu-%s.lock",
        (unsigned long)getuid(),
        shm_name + 1
    );
    if (written <= 0 || (size_t)written >= sizeof(path)) {
        return DATAROUTER_ERR_INVALID_ARGUMENT;
    }
    int fd = open(path, O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) {
        return errno == EACCES || errno == EPERM
            ? DATAROUTER_ERR_PERMISSION
            : DATAROUTER_ERR_PLATFORM;
    }
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_uid != getuid() ||
        fchmod(fd, 0600) != 0) {
        close(fd);
        return DATAROUTER_ERR_PERMISSION;
    }
    if (flock(fd, LOCK_EX) != 0) {
        close(fd);
        return DATAROUTER_ERR_PLATFORM;
    }
    lock->fd = fd;
    return DATAROUTER_OK;
#endif
}

static void release_start_lock(switch_start_lock *lock) {
    if (!lock) {
        return;
    }
#ifndef _WIN32
    if (lock->fd >= 0) {
        (void)flock(lock->fd, LOCK_UN);
        close(lock->fd);
        lock->fd = -1;
    }
#endif
}

static void handle_signal(int signum) {
    (void)signum;
    g_stop = 1;
}

static void print_usage(const char *argv0) {
    fprintf(
        stderr,
        "usage: %s [--name /datarouter] [--mode 0600] [--ttl-ms 30000] "
        "(--allow Core,MLHelper | --config path.ini | --allow-any) [--once] "
        "[--reap-dead-processes] "
        "[--log-level error|warn|info|debug|trace]\n",
        argv0
    );
}

static const char *log_level_name(switch_log_level level) {
    switch (level) {
        case SWITCH_LOG_ERROR:
            return "error";
        case SWITCH_LOG_WARN:
            return "warn";
        case SWITCH_LOG_INFO:
            return "info";
        case SWITCH_LOG_DEBUG:
            return "debug";
        case SWITCH_LOG_TRACE:
            return "trace";
        default:
            return "info";
    }
}

static void switch_log(const switch_options *options, switch_log_level level, const char *fmt, ...) {
    if (level > options->log_level) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "datarouter-switch[%s]: ", log_level_name(level));
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
}

static int parse_log_level(const char *value, switch_log_level *out) {
    if (strcmp(value, "error") == 0) {
        *out = SWITCH_LOG_ERROR;
    } else if (strcmp(value, "warn") == 0) {
        *out = SWITCH_LOG_WARN;
    } else if (strcmp(value, "info") == 0) {
        *out = SWITCH_LOG_INFO;
    } else if (strcmp(value, "debug") == 0) {
        *out = SWITCH_LOG_DEBUG;
    } else if (strcmp(value, "trace") == 0) {
        *out = SWITCH_LOG_TRACE;
    } else {
        return DATAROUTER_ERR_INVALID_ARGUMENT;
    }
    return DATAROUTER_OK;
}

static int parse_u32_strict(const char *value, uint32_t min, uint32_t max, uint32_t *out) {
    if (!value || !out || value[0] == '\0' || value[0] == '-' || value[0] == '+') {
        return DATAROUTER_ERR_INVALID_ARGUMENT;
    }
    errno = 0;
    char *end = NULL;
    unsigned long parsed = strtoul(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0' || parsed < min || parsed > max) {
        return DATAROUTER_ERR_INVALID_ARGUMENT;
    }
    *out = (uint32_t)parsed;
    return DATAROUTER_OK;
}

static int parse_mode_strict(const char *value, mode_t *out) {
    if (!value || !out || value[0] == '\0' || value[0] == '-' || value[0] == '+') {
        return DATAROUTER_ERR_INVALID_ARGUMENT;
    }
    errno = 0;
    char *end = NULL;
    unsigned long parsed = strtoul(value, &end, 8);
    if (errno != 0 || end == value || *end != '\0' || parsed > 0777UL ||
        (parsed & 0600UL) != 0600UL || (parsed & 0111UL) != 0UL) {
        return DATAROUTER_ERR_INVALID_ARGUMENT;
    }
    *out = (mode_t)parsed;
    return DATAROUTER_OK;
}

static int allowlist_contains(const switch_options *options, const char *service_name) {
    for (uint32_t i = 0U; i < options->allowlist_len; i++) {
        if (strcmp(options->allowlist[i], service_name) == 0) {
            return 1;
        }
    }
    return 0;
}

static int parse_allowlist(switch_options *options, const char *value) {
    if (!value || value[0] == '\0' || strlen(value) >= 512U) {
        return DATAROUTER_ERR_INVALID_ARGUMENT;
    }
    char copy[512];
    memcpy(copy, value, strlen(value) + 1U);
    char *cursor = copy;
    while (1) {
        char *comma = strchr(cursor, ',');
        if (comma) {
            *comma = '\0';
        }
        if (!datarouter_valid_service_name(cursor) || allowlist_contains(options, cursor)) {
            return DATAROUTER_ERR_INVALID_ARGUMENT;
        }
        if (options->allowlist_len >= DATAROUTER_MAX_PORTS) {
            return DATAROUTER_ERR_FULL;
        }
        snprintf(
            options->allowlist[options->allowlist_len],
            sizeof(options->allowlist[options->allowlist_len]),
            "%s",
            cursor
        );
        options->allowlist_len++;
        if (!comma) {
            break;
        }
        cursor = comma + 1;
    }
    return DATAROUTER_OK;
}

static int service_allowed(const switch_options *options, const char *service_name) {
    if (options->allow_any) {
        return 1;
    }
    if (options->config_mode) {
        for (uint32_t i = 0U; i < options->config.allow_port_count; i++) {
            if (strcmp(options->config.allow_port[i], service_name) == 0) {
                return 1;
            }
        }
        return 0;
    }
    return allowlist_contains(options, service_name);
}

static int parse_args(int argc, char **argv, switch_options *options) {
    memset(options, 0, sizeof(*options));
    options->shm_name = DATAROUTER_DEFAULT_SHM_NAME;
    options->mode = 0600;
    options->ttl_ms = DATAROUTER_DEFAULT_TTL_MS;
    options->log_level = SWITCH_LOG_INFO;
    datarouter_config_init(&options->config);

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--name") == 0 && i + 1 < argc) {
            options->shm_name = argv[++i];
        } else if (strcmp(argv[i], "--mode") == 0 && i + 1 < argc) {
            if (parse_mode_strict(argv[++i], &options->mode) != DATAROUTER_OK) {
                return DATAROUTER_ERR_INVALID_ARGUMENT;
            }
        } else if (strcmp(argv[i], "--ttl-ms") == 0 && i + 1 < argc) {
            if (parse_u32_strict(argv[++i], 1U, 600000U, &options->ttl_ms) != DATAROUTER_OK) {
                return DATAROUTER_ERR_INVALID_ARGUMENT;
            }
        } else if (strcmp(argv[i], "--allow") == 0 && i + 1 < argc) {
            if (options->config_mode || options->allow_any || options->allowlist_len != 0U ||
                parse_allowlist(options, argv[++i]) != DATAROUTER_OK) {
                return DATAROUTER_ERR_INVALID_ARGUMENT;
            }
        } else if (strcmp(argv[i], "--allow-any") == 0) {
            if (options->config_mode || options->allow_any || options->allowlist_len != 0U) {
                return DATAROUTER_ERR_INVALID_ARGUMENT;
            }
            options->allow_any = 1;
        } else if (strcmp(argv[i], "--config") == 0 && i + 1 < argc) {
            if (options->config_mode || options->allow_any || options->allowlist_len != 0U) {
                return DATAROUTER_ERR_INVALID_ARGUMENT;
            }
            options->config_path = argv[++i];
            options->config_mode = 1;
        } else if (strcmp(argv[i], "--once") == 0) {
            options->once = 1;
        } else if (strcmp(argv[i], "--reap-dead-processes") == 0) {
            options->reap_dead_processes = 1;
        } else if (strcmp(argv[i], "--log-level") == 0 && i + 1 < argc) {
            if (parse_log_level(argv[++i], &options->log_level) != DATAROUTER_OK) {
                return DATAROUTER_ERR_INVALID_ARGUMENT;
            }
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_usage(argv[0]);
            exit(0);
        } else {
            return DATAROUTER_ERR_INVALID_ARGUMENT;
        }
    }
    if (!datarouter_valid_shm_name(options->shm_name)) {
        return DATAROUTER_ERR_INVALID_ARGUMENT;
    }
    if (options->config_mode) {
        char err[128];
        int rc = datarouter_config_load(options->config_path, &options->config, err, sizeof(err));
        if (rc != DATAROUTER_OK) {
            fprintf(stderr, "datarouter-switch: config error: %s\n", err);
            return rc;
        }
    } else if (!options->allow_any && options->allowlist_len == 0U) {
        fprintf(stderr, "datarouter-switch: an allowlist is required; use --allow-any explicitly\n");
        return DATAROUTER_ERR_PERMISSION;
    }
    return DATAROUTER_OK;
}

static void describe_allowlist(const switch_options *options, char *out, size_t out_len) {
    if (!out || out_len == 0U) {
        return;
    }
    if (options->allow_any) {
        snprintf(out, out_len, "allow:any(explicit)");
        return;
    }
    if (options->config_mode) {
        snprintf(out, out_len, "config:%s", options->config_path);
        return;
    }
    size_t used = (size_t)snprintf(out, out_len, "allow:");
    for (uint32_t i = 0U; i < options->allowlist_len && used < out_len; i++) {
        int written = snprintf(
            out + used,
            out_len - used,
            "%s%s",
            i == 0U ? "" : ",",
            options->allowlist[i]
        );
        if (written < 0 || (size_t)written >= out_len - used) {
            out[out_len - 1U] = '\0';
            return;
        }
        used += (size_t)written;
    }
}

static uint32_t active_port_count(datarouter_shared_memory *shm) {
    uint32_t active = 0U;
    uint32_t max_ports = datarouter_active_max_ports(shm);
    for (uint32_t i = 0U; i < max_ports; i++) {
        if (atomic_load_explicit(&shm->control.registry[i].state, memory_order_acquire) ==
            DATAROUTER_PORT_ACTIVE) {
            active++;
        }
    }
    return active;
}

static int request_claim_owner_alive(
    const datarouter_shared_memory *shm,
    unsigned long long claim
) {
    if (!shm || claim == 0ULL) {
        return 0;
    }
    pid_t owner_pid = (pid_t)(uint32_t)(claim >> 32U);
    uint32_t actual_uid = 0U;
    uint64_t actual_start = 0ULL;
    if (datarouter_process_identity(owner_pid, &actual_uid, &actual_start) != DATAROUTER_OK) {
        return 0;
    }
    uint32_t actual_identity = (uint32_t)(actual_start ^ (actual_start >> 32U));
    if (actual_identity == 0U) {
        actual_identity = 1U;
    }
    return actual_uid == shm->control.switch_uid && actual_identity == (uint32_t)claim;
}

static void recover_control_request_slot(
    datarouter_shared_memory *shm,
    datarouter_register_request *req
) {
    unsigned int state = atomic_load_explicit(&req->state, memory_order_acquire);
    if (state == DATAROUTER_REG_PROCESSING || state == DATAROUTER_REG_ABANDONED ||
        state == DATAROUTER_REG_RECOVERING) {
        return;
    }
    if (state == DATAROUTER_REG_EMPTY) {
        unsigned long long claim = atomic_load_explicit(
            &req->claim_token,
            memory_order_acquire
        );
        if (claim == 0ULL || request_claim_owner_alive(shm, claim)) {
            return;
        }
        if (atomic_compare_exchange_strong_explicit(
                &req->claim_token,
                &claim,
                0ULL,
                memory_order_acq_rel,
                memory_order_acquire
            )) {
            atomic_fetch_add_explicit(
                &shm->control.registration_request_recoveries,
                1ULL,
                memory_order_relaxed
            );
        }
        return;
    }
    unsigned long long observed_claim = atomic_load_explicit(
        &req->claim_token,
        memory_order_acquire
    );
    if (observed_claim == 0ULL || request_claim_owner_alive(shm, observed_claim)) {
        return;
    }
    unsigned int observed_state = state;
    if (!atomic_compare_exchange_strong_explicit(
            &req->state,
            &state,
            DATAROUTER_REG_RECOVERING,
            memory_order_acq_rel,
            memory_order_acquire
        )) {
        return;
    }
    unsigned long long claim = atomic_load_explicit(&req->claim_token, memory_order_acquire);
    if (claim != observed_claim || request_claim_owner_alive(shm, claim)) {
        unsigned int recovering = DATAROUTER_REG_RECOVERING;
        (void)atomic_compare_exchange_strong_explicit(
            &req->state,
            &recovering,
            observed_state,
            memory_order_release,
            memory_order_acquire
        );
        return;
    }

    unsigned int recovering = DATAROUTER_REG_RECOVERING;
    if (atomic_compare_exchange_strong_explicit(
            &req->state,
            &recovering,
            DATAROUTER_REG_EMPTY,
            memory_order_acq_rel,
            memory_order_acquire
        )) {
        if (claim != 0ULL) {
            (void)atomic_compare_exchange_strong_explicit(
                &req->claim_token,
                &claim,
                0ULL,
                memory_order_release,
                memory_order_acquire
            );
        }
        if (claim != 0ULL) {
            atomic_fetch_add_explicit(
                &shm->control.registration_request_recoveries,
                1ULL,
                memory_order_relaxed
            );
        }
    }
}

static void recover_control_requests(datarouter_shared_memory *shm) {
    for (uint32_t i = 0U; i < DATAROUTER_REGISTRATION_REQUEST_SLOTS; i++) {
        recover_control_request_slot(shm, &shm->control.register_requests[i]);
    }
}

static int request_identity_allowed(
    const datarouter_shared_memory *shm,
    const datarouter_register_request *req
) {
    uint32_t actual_uid = 0U;
    uint64_t actual_start = 0ULL;
    return req->owner_pid > 0 && req->owner_start_id != 0ULL &&
           datarouter_process_identity((pid_t)req->owner_pid, &actual_uid, &actual_start) ==
               DATAROUTER_OK &&
           actual_uid == req->owner_uid && actual_start == req->owner_start_id &&
           actual_uid == shm->control.switch_uid;
}

static unsigned int finish_request(
    datarouter_register_request *req,
    unsigned int state,
    int code,
    const char *message
) {
    req->result_code = code;
    req->response_nonce = req->request_nonce;
    snprintf(req->message, sizeof(req->message), "%s", message ? message : datarouter_strerror(code));
    return state;
}

static int find_free_slot(datarouter_shared_memory *shm, uint32_t *slot_index) {
    uint32_t max_ports = datarouter_active_max_ports(shm);
    for (uint32_t i = 0U; i < max_ports; i++) {
        if (datarouter_registry_empty_slot_ready(&shm->control.registry[i])) {
            *slot_index = i;
            return DATAROUTER_OK;
        }
    }
    return DATAROUTER_ERR_FULL;
}

static int service_name_reserved(datarouter_shared_memory *shm, const char *service_name) {
    uint32_t max_ports = datarouter_active_max_ports(shm);
    for (uint32_t i = 0U; i < max_ports; i++) {
        datarouter_registry_entry *entry = &shm->control.registry[i];
        if (atomic_load_explicit(&entry->state, memory_order_acquire) == DATAROUTER_PORT_EMPTY) {
            continue;
        }
        char existing[DATAROUTER_SERVICE_NAME_MAX + 1U];
        datarouter_registry_copy_name(entry, existing);
        if (strcmp(existing, service_name) == 0) {
            return 1;
        }
    }
    return 0;
}

static void activate_slot(
    datarouter_shared_memory *shm,
    const datarouter_register_request *req,
    uint32_t slot_index
) {
    datarouter_registry_entry *entry = &shm->control.registry[slot_index];
    datarouter_ring_reset(datarouter_rx_channel(shm, slot_index));
    datarouter_ring_reset(datarouter_tx_channel(shm, slot_index));
    uint32_t generation = (entry->generation + 1U) & DATAROUTER_ROUTE_GENERATION_MASK;
    if (generation == 0U) {
        generation = 1U;
    }
    entry->slot_index = slot_index;
    entry->generation = generation;
    atomic_store_explicit(
        &entry->route_id,
        datarouter_route_for_slot_generation(slot_index, generation),
        memory_order_relaxed
    );
    atomic_store_explicit(&entry->pid, req->owner_pid, memory_order_relaxed);
    atomic_store_explicit(&entry->uid, req->owner_uid, memory_order_relaxed);
    atomic_store_explicit(&entry->process_start_id, req->owner_start_id, memory_order_relaxed);
    datarouter_registry_store_name(entry, req->service_name);
    atomic_store_explicit(&entry->last_active_ns, datarouter_now_ns(), memory_order_release);
    atomic_store_explicit(&entry->state, DATAROUTER_PORT_ACTIVE, memory_order_seq_cst);
}

static unsigned int handle_register_request(
    datarouter_shared_memory *shm,
    const switch_options *options,
    datarouter_register_request *req
) {
    if (!datarouter_valid_service_name(req->service_name)) {
        return finish_request(req, DATAROUTER_REG_REJECTED, DATAROUTER_ERR_INVALID_ARGUMENT, "invalid service name");
    }
    if (!service_allowed(options, req->service_name)) {
        return finish_request(req, DATAROUTER_REG_REJECTED, DATAROUTER_ERR_PERMISSION, "service is not in switch allowlist");
    }
    if (!request_identity_allowed(shm, req)) {
        return finish_request(req, DATAROUTER_REG_REJECTED, DATAROUTER_ERR_PERMISSION, "pid, uid, or process start identity is invalid");
    }
    if (service_name_reserved(shm, req->service_name)) {
        return finish_request(req, DATAROUTER_REG_REJECTED, DATAROUTER_ERR_BUSY, "service is already registered");
    }
    uint32_t slot_index = 0U;
    int rc = find_free_slot(shm, &slot_index);
    if (rc != DATAROUTER_OK) {
        return finish_request(req, DATAROUTER_REG_REJECTED, rc, "no free datarouter slots");
    }
    activate_slot(shm, req, slot_index);
    req->assigned_slot = slot_index;
    req->assigned_route_id = atomic_load_explicit(
        &shm->control.registry[slot_index].route_id,
        memory_order_acquire
    );
    return finish_request(req, DATAROUTER_REG_ACCEPTED, DATAROUTER_OK, "ok");
}

static unsigned int handle_unregister_request(
    datarouter_shared_memory *shm,
    datarouter_register_request *req
) {
    if (!request_identity_allowed(shm, req) || !datarouter_valid_service_name(req->service_name) ||
        req->route_id == 0U) {
        return finish_request(req, DATAROUTER_REG_REJECTED, DATAROUTER_ERR_PERMISSION, "invalid unregister owner");
    }
    datarouter_registry_metadata metadata;
    if (!datarouter_registry_find_active_metadata_by_route(shm, req->route_id, &metadata) ||
        metadata.pid != req->owner_pid || metadata.uid != req->owner_uid ||
        metadata.process_start_id != req->owner_start_id ||
        strcmp(metadata.service_name, req->service_name) != 0) {
        return finish_request(req, DATAROUTER_REG_REJECTED, DATAROUTER_ERR_NOT_FOUND, "registration no longer belongs to caller");
    }
    uint32_t slot = metadata.slot_index;
    if (!datarouter_registry_begin_drain(shm, slot)) {
        return finish_request(req, DATAROUTER_REG_REJECTED, DATAROUTER_ERR_BUSY, "registration is already closing");
    }
    req->assigned_slot = slot;
    req->assigned_route_id = 0U;
    return finish_request(req, DATAROUTER_REG_ACCEPTED, DATAROUTER_OK, "ok");
}

static void handle_control_request_slot(
    datarouter_shared_memory *shm,
    const switch_options *options,
    datarouter_register_request *req
) {
    unsigned int pending = DATAROUTER_REG_PENDING;
    if (!atomic_compare_exchange_strong_explicit(
            &req->state,
            &pending,
            DATAROUTER_REG_PROCESSING,
            memory_order_acq_rel,
            memory_order_acquire
        )) {
        return;
    }
    while (!datarouter_try_lock(&shm->control.control_lock)) {
        datarouter_yield();
    }
    uint64_t now = datarouter_now_ns();
    unsigned int terminal_state = DATAROUTER_REG_REJECTED;
    if (req->request_nonce == 0ULL || req->deadline_ns == 0ULL || now >= req->deadline_ns) {
        terminal_state = finish_request(req, DATAROUTER_REG_REJECTED, DATAROUTER_ERR_TIMEOUT, "control request expired");
    } else if (req->operation == DATAROUTER_REG_OP_REGISTER) {
        terminal_state = handle_register_request(shm, options, req);
    } else if (req->operation == DATAROUTER_REG_OP_UNREGISTER) {
        terminal_state = handle_unregister_request(shm, req);
    } else {
        terminal_state = finish_request(req, DATAROUTER_REG_REJECTED, DATAROUTER_ERR_PROTOCOL, "unknown control operation");
    }

    /* Snapshot diagnostics before publishing the terminal state. Logging can
     * be slow, so it happens from these locals after the client may reuse the
     * mailbox. */
    uint32_t operation = req->operation;
    int result_code = req->result_code;
    int32_t owner_pid = req->owner_pid;
    uint32_t owner_uid = req->owner_uid;
    uint32_t assigned_slot = req->assigned_slot;
    uint32_t assigned_route_id = req->assigned_route_id;
    char service_name[DATAROUTER_SERVICE_NAME_MAX + 1U];
    char result_message[sizeof(req->message)];
    snprintf(service_name, sizeof(service_name), "%s", req->service_name);
    snprintf(result_message, sizeof(result_message), "%s", req->message);

    /* This is the release publication point for every response. Do not read
     * request payload fields after it: the client may immediately consume,
     * clear, and reuse this mailbox. */
    int publication = datarouter_registry_publish_control_response(
        shm,
        req,
        terminal_state,
        operation,
        result_code,
        assigned_slot
    );
    int abandoned = publication == 0;
    datarouter_unlock(&shm->control.control_lock);
    datarouter_signal_post(&shm->control.registration_signal);

    if (operation == DATAROUTER_REG_OP_REGISTER) {
        const char *action = abandoned
            ? (result_code == DATAROUTER_OK ? "rollback" : "abandoned")
            : (result_code == DATAROUTER_OK ? "accept" : "reject");
        switch_log(
            options,
            result_code == DATAROUTER_OK && !abandoned ? SWITCH_LOG_INFO : SWITCH_LOG_WARN,
            "registration %s service=%s pid=%d uid=%u slot=%u route=%u code=%d message=%s",
            action,
            service_name,
            (int)owner_pid,
            owner_uid,
            assigned_slot,
            assigned_route_id,
            result_code,
            result_message
        );
    }
}

static void handle_control_requests(
    datarouter_shared_memory *shm,
    const switch_options *options
) {
    uint64_t ready = atomic_exchange_explicit(
        &shm->control.request_ready_mask,
        0ULL,
        memory_order_acq_rel
    );
    while (ready != 0ULL) {
        uint32_t i = (uint32_t)__builtin_ctzll(ready);
        ready &= ready - 1ULL;
        handle_control_request_slot(shm, options, &shm->control.register_requests[i]);
    }
}

static void process_corrupt_slots(datarouter_shared_memory *shm, const switch_options *options) {
    uint64_t corrupt = datarouter_scheduler_take_corrupt(shm);
    if (corrupt == 0ULL) {
        return;
    }
    while (!datarouter_try_lock(&shm->control.control_lock)) {
        datarouter_yield();
    }
    uint32_t max_ports = datarouter_active_max_ports(shm);
    for (uint32_t slot = 0U; slot < max_ports; slot++) {
        if ((corrupt & (1ULL << slot)) == 0ULL) {
            continue;
        }
        datarouter_registry_metadata metadata;
        if (!datarouter_registry_read_active(&shm->control.registry[slot], &metadata)) {
            continue;
        }
        switch_log(
            options,
            SWITCH_LOG_ERROR,
            "isolating corrupted slot=%u service=%s route=%u",
            slot,
            metadata.service_name,
            metadata.route_id
        );
        atomic_fetch_add_explicit(&shm->control.corrupted_rings, 1ULL, memory_order_relaxed);
        atomic_fetch_add_explicit(&shm->control.registration_recoveries, 1ULL, memory_order_relaxed);
        (void)datarouter_registry_release_slot(shm, slot);
        datarouter_signal_post(&shm->control.rx_signal[slot]);
    }
    datarouter_unlock(&shm->control.control_lock);
}

static void reap_ttl(datarouter_shared_memory *shm, const switch_options *options) {
    if (!datarouter_try_lock(&shm->control.control_lock)) {
        return;
    }
    uint64_t now = datarouter_now_ns();
    uint64_t ttl_ns = (uint64_t)options->ttl_ms * 1000000ULL;
    uint32_t max_ports = datarouter_active_max_ports(shm);
    for (uint32_t i = 0U; i < max_ports; i++) {
        datarouter_registry_entry *entry = &shm->control.registry[i];
        if (atomic_load_explicit(&entry->state, memory_order_acquire) != DATAROUTER_PORT_ACTIVE) {
            continue;
        }
        datarouter_registry_metadata metadata;
        if (!datarouter_registry_read_active(entry, &metadata)) {
            continue;
        }
        uint64_t last = metadata.last_active_ns;
        if (last == 0ULL || last > now || now - last <= ttl_ns) {
            continue;
        }
        switch_log(
            options,
            SWITCH_LOG_INFO,
            "ttl reap service=%s slot=%u pid=%d age_ms=%" PRIu64 " ttl_ms=%u",
            metadata.service_name,
            metadata.slot_index,
            (int)metadata.pid,
            (now - last) / 1000000ULL,
            options->ttl_ms
        );
        (void)datarouter_registry_release_slot(shm, i);
    }
    datarouter_unlock(&shm->control.control_lock);
}

/* A failed identity probe is not evidence of death: /proc can be transiently
 * unavailable or access can be denied. Reap immediately only when an exact
 * owner identity disappeared or was replaced by another uid/start identity.
 * A stopped JVM therefore remains registered until its TTL expires. */
static int process_identity_proves_dead(const datarouter_registry_metadata *metadata) {
    if (!metadata || metadata->pid <= 0 || metadata->process_start_id == 0ULL) {
        return 0;
    }
    uint32_t actual_uid = 0U;
    uint64_t actual_start_id = 0ULL;
    int rc = datarouter_process_identity(
        (pid_t)metadata->pid,
        &actual_uid,
        &actual_start_id
    );
    if (rc == DATAROUTER_ERR_NOT_FOUND) {
        return 1;
    }
    return rc == DATAROUTER_OK &&
           (actual_uid != metadata->uid || actual_start_id != metadata->process_start_id);
}

static void reap_dead_processes(datarouter_shared_memory *shm, const switch_options *options) {
    if (!options->reap_dead_processes || !datarouter_try_lock(&shm->control.control_lock)) {
        return;
    }
    uint32_t max_ports = datarouter_active_max_ports(shm);
    for (uint32_t i = 0U; i < max_ports; i++) {
        datarouter_registry_entry *entry = &shm->control.registry[i];
        datarouter_registry_metadata metadata;
        if (atomic_load_explicit(&entry->state, memory_order_acquire) != DATAROUTER_PORT_ACTIVE ||
            !datarouter_registry_read_active(entry, &metadata) ||
            !process_identity_proves_dead(&metadata)) {
            continue;
        }
        if (datarouter_registry_release_slot(shm, i)) {
            atomic_fetch_add_explicit(
                &shm->control.dead_process_reaps,
                1ULL,
                memory_order_relaxed
            );
            switch_log(
                options,
                SWITCH_LOG_INFO,
                "dead-process reap service=%s slot=%u pid=%d",
                metadata.service_name,
                metadata.slot_index,
                (int)metadata.pid
            );
        }
    }
    datarouter_unlock(&shm->control.control_lock);
}

static int existing_switch_is_live(const char *name, int *live) {
    *live = 0;
    datarouter_mapping existing;
    int rc = datarouter_platform_open(name, 0U, &existing);
    if (rc != DATAROUTER_OK) {
        return rc;
    }
    if (existing.map_size < sizeof(datarouter_control_plane)) {
        datarouter_platform_close(&existing);
        return DATAROUTER_ERR_PROTOCOL;
    }
    if (existing.shm->control.magic == 0U && existing.shm->control.layout_version == 0U &&
        existing.shm->control.switch_pid == 0) {
        datarouter_platform_close(&existing);
        return DATAROUTER_OK;
    }
    if (existing.shm->control.magic != DATAROUTER_MAGIC ||
        existing.shm->control.layout_version != DATAROUTER_LAYOUT_VERSION) {
        datarouter_platform_close(&existing);
        return DATAROUTER_ERR_PROTOCOL;
    }
    *live = datarouter_process_identity_matches(
        (pid_t)existing.shm->control.switch_pid,
        existing.shm->control.switch_uid,
        existing.shm->control.switch_start_id
    );
    datarouter_platform_close(&existing);
    return DATAROUTER_OK;
}

static int create_switch_mapping(
    const switch_options *options,
    size_t size,
    datarouter_mapping *mapping
) {
    for (uint32_t attempt = 0U; attempt < DATAROUTER_CREATE_RETRIES; attempt++) {
        int rc = datarouter_platform_create(options->shm_name, options->mode, size, mapping);
        if (rc == DATAROUTER_OK) {
            return DATAROUTER_OK;
        }
        if (rc != DATAROUTER_ERR_BUSY) {
            return rc;
        }
        int live = 0;
        rc = existing_switch_is_live(options->shm_name, &live);
        if (rc != DATAROUTER_OK) {
            return rc;
        }
        if (live) {
            return DATAROUTER_ERR_BUSY;
        }
        rc = datarouter_platform_unlink(options->shm_name);
        if (rc != DATAROUTER_OK) {
            return rc;
        }
    }
    return DATAROUTER_ERR_BUSY;
}

static void log_active_entries(datarouter_shared_memory *shm, const switch_options *options, uint64_t now) {
    uint32_t max_ports = datarouter_active_max_ports(shm);
    for (uint32_t i = 0U; i < max_ports; i++) {
        datarouter_registry_entry *entry = &shm->control.registry[i];
        if (atomic_load_explicit(&entry->state, memory_order_acquire) != DATAROUTER_PORT_ACTIVE) {
            continue;
        }
        datarouter_registry_metadata metadata;
        if (!datarouter_registry_read_active(entry, &metadata)) {
            continue;
        }
        uint64_t last = metadata.last_active_ns;
        switch_log(
            options,
            SWITCH_LOG_DEBUG,
            "active service=%s slot=%u pid=%d age_ms=%" PRIu64,
            metadata.service_name,
            metadata.slot_index,
            (int)metadata.pid,
            last > 0ULL && now >= last ? (now - last) / 1000000ULL : 0ULL
        );
    }
}

int main(int argc, char **argv) {
    switch_options options;
    int rc = parse_args(argc, argv, &options);
    if (rc != DATAROUTER_OK) {
        print_usage(argv[0]);
        fprintf(stderr, "datarouter-switch: %s\n", datarouter_strerror(rc));
        return 2;
    }
    if (options.mode != 0600) {
        switch_log(
            &options,
            SWITCH_LOG_WARN,
            "shared memory mode %04o is broader than the recommended production mode 0600; "
            "registration remains restricted to uid=%u",
            (unsigned int)options.mode,
#ifdef _WIN32
            0U
#else
            (unsigned int)getuid()
#endif
        );
    }

    uint32_t max_ports = options.config_mode
        ? options.config.allow_port_count
        : DATAROUTER_DEFAULT_MAX_PORTS;
    size_t map_size = datarouter_shared_memory_size(max_ports);
    datarouter_mapping mapping;
    switch_start_lock start_lock;
    rc = acquire_start_lock(options.shm_name, &start_lock);
    if (rc != DATAROUTER_OK) {
        fprintf(stderr, "datarouter-switch: acquire startup lock failed: %s\n", datarouter_strerror(rc));
        return 1;
    }
    rc = create_switch_mapping(&options, map_size, &mapping);
    if (rc != DATAROUTER_OK) {
        release_start_lock(&start_lock);
        fprintf(stderr, "datarouter-switch: create shm failed: %s\n", datarouter_strerror(rc));
        return 1;
    }
    rc = datarouter_init_shared_memory_with_ttl(mapping.shm, max_ports, options.ttl_ms);
    if (rc != DATAROUTER_OK) {
        fprintf(stderr, "datarouter-switch: init shm failed: %s\n", datarouter_strerror(rc));
        datarouter_platform_close(&mapping);
        (void)datarouter_platform_unlink(options.shm_name);
        release_start_lock(&start_lock);
        return 1;
    }
    release_start_lock(&start_lock);

    char allow_desc[512];
    describe_allowlist(&options, allow_desc, sizeof(allow_desc));
    switch_log(
        &options,
        SWITCH_LOG_INFO,
        "start pid=%d start_id=%" PRIu64 " shm=%s mode=%04o ttl_ms=%u max_ports=%u "
        "%s once=%d reap_dead_processes=%d version=%s layout=%u channel_size=%u",
        (int)switch_pid(),
        mapping.shm->control.switch_start_id,
        options.shm_name,
        (unsigned int)options.mode,
        options.ttl_ms,
        max_ports,
        allow_desc,
        options.once,
        options.reap_dead_processes,
        datarouter_version(),
        DATAROUTER_LAYOUT_VERSION,
        DATAROUTER_CHANNEL_SIZE
    );
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    datarouter_route_stats route_stats = {0};
    uint64_t active_spin_started_ns = 0ULL;
    uint64_t idle_spin_started_ns = 0ULL;
    uint64_t next_reap_ns = 0ULL;
    uint64_t next_dead_reap_ns = 0ULL;
    uint64_t next_request_recovery_ns = 0ULL;
    uint64_t next_stats_ns = datarouter_now_ns() + 5000000000ULL;
    uint64_t reap_interval_ns = ((uint64_t)options.ttl_ms * 1000000ULL) / 2ULL;
    if (reap_interval_ns < 1000000ULL) {
        reap_interval_ns = 1000000ULL;
    } else if (reap_interval_ns > 1000000000ULL) {
        reap_interval_ns = 1000000000ULL;
    }

    do {
        uint64_t iteration_started_ns = datarouter_now_ns();
        if (iteration_started_ns >= next_request_recovery_ns) {
            recover_control_requests(mapping.shm);
            next_request_recovery_ns = iteration_started_ns + 1000000ULL;
        }
        datarouter_registry_cleanup_releasing(mapping.shm);
        process_corrupt_slots(mapping.shm, &options);
        handle_control_requests(mapping.shm, &options);
        uint32_t routed = datarouter_route_frames_stats(mapping.shm, &route_stats);
        uint64_t now = datarouter_now_ns();
        if (options.reap_dead_processes && now >= next_dead_reap_ns) {
            reap_dead_processes(mapping.shm, &options);
            /* This recovery path is deliberately independent of TTL.  It
             * makes a confirmed dead process reclaimable promptly without
             * probing live ports in the router hot loop. */
            next_dead_reap_ns = now + 10000000ULL;
        }
        if (now >= next_reap_ns) {
            reap_ttl(mapping.shm, &options);
            next_reap_ns = now + reap_interval_ns;
        }
        if (options.log_level >= SWITCH_LOG_DEBUG && now >= next_stats_ns) {
            switch_log(
                &options,
                SWITCH_LOG_DEBUG,
                "stats routed=%" PRIu64 " full=%" PRIu64 " not_found=%" PRIu64
                " protocol=%" PRIu64 " corrupted=%" PRIu64 " internal=%" PRIu64
                " active_ports=%u blocked_wakeups=%" PRIu64,
                route_stats.routed,
                route_stats.full,
                route_stats.not_found,
                route_stats.protocol,
                route_stats.corrupted,
                route_stats.internal,
                active_port_count(mapping.shm),
                atomic_load_explicit(&mapping.shm->control.blocked_wakeups, memory_order_relaxed)
            );
            log_active_entries(mapping.shm, &options, now);
            next_stats_ns = now + 5000000000ULL;
        }

        if (routed > 0U) {
            idle_spin_started_ns = 0ULL;
            if (active_spin_started_ns == 0ULL) {
                active_spin_started_ns = now;
            }
            if (now - active_spin_started_ns >=
                (uint64_t)DATAROUTER_SWITCH_SPIN_BUDGET_US * 1000ULL) {
                active_spin_started_ns = 0ULL;
                datarouter_yield();
            }
            continue;
        }
        active_spin_started_ns = 0ULL;
        if (!options.once) {
            now = datarouter_now_ns();
            if (idle_spin_started_ns == 0ULL) {
                idle_spin_started_ns = now;
            }
            if (now - idle_spin_started_ns <
                (uint64_t)DATAROUTER_SWITCH_SPIN_BUDGET_US * 1000ULL) {
                continue;
            }
            idle_spin_started_ns = 0ULL;
            (void)datarouter_signal_wait(
                &mapping.shm->control.switch_signal,
                DATAROUTER_SWITCH_WAIT_MS
            );
        }
    } while (!g_stop && !options.once);

    switch_log(
        &options,
        SWITCH_LOG_INFO,
        "exit reason=%s routed=%" PRIu64 " full=%" PRIu64 " not_found=%" PRIu64
        " protocol=%" PRIu64 " corrupted=%" PRIu64 " internal=%" PRIu64,
        g_stop ? "signal" : "once",
        route_stats.routed,
        route_stats.full,
        route_stats.not_found,
        route_stats.protocol,
        route_stats.corrupted,
        route_stats.internal
    );
    datarouter_platform_close(&mapping);
    rc = datarouter_platform_unlink(options.shm_name);
    if (rc != DATAROUTER_OK) {
        fprintf(stderr, "datarouter-switch: unlink shm failed: %s\n", datarouter_strerror(rc));
        return 1;
    }
    return 0;
}
