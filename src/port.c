#include "wait_policy.h"
#include "datarouter_internal.h"
#include "datarouter_registry.h"
#include "datarouter_scheduler.h"
#include "datarouter_signal.h"

#include <stdatomic.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

#include "wait_diagnostics.h"
#if defined(DATAROUTER_WAIT_DIAGNOSTICS) && !defined(_WIN32)
extern void datarouter_diagnostic_flush(void);
#endif

#define DATAROUTER_UNREGISTER_TIMEOUT_MS 100U

static atomic_ullong g_request_nonce = 1ULL;

static uint32_t option_timeout_ms(const datarouter_options *options) {
    return options && options->timeout_ms > 0U
        ? options->timeout_ms
        : DATAROUTER_DEFAULT_TIMEOUT_MS;
}

static const char *option_shm_name(const datarouter_options *options) {
    return options && options->shm_name && options->shm_name[0] != '\0'
        ? options->shm_name
        : DATAROUTER_DEFAULT_SHM_NAME;
}

static int32_t current_pid(void) {
#ifdef _WIN32
    return (int32_t)_getpid();
#else
    return (int32_t)getpid();
#endif
}

static uint32_t current_uid(void) {
#ifdef _WIN32
    return 0U;
#else
    return (uint32_t)getuid();
#endif
}

static int port_tx_mutex_init(datarouter_port *port) {
    if (!port) return DATAROUTER_ERR_INVALID_ARGUMENT;
    atomic_init(&port->tx_mutex.state, 0U);
    return DATAROUTER_OK;
}
static int port_tx_mutex_lock(datarouter_port *port) {
    if (!port) return DATAROUTER_ERR_INVALID_ARGUMENT;
    while (!datarouter_try_lock(&port->tx_mutex.state)) datarouter_cpu_pause();
    return DATAROUTER_OK;
}
static void port_tx_mutex_unlock(datarouter_port *port) {
    datarouter_unlock(&port->tx_mutex.state);
}
static void port_tx_mutex_destroy(datarouter_port *port) { (void)port; }
static int port_rx_mutex_init(datarouter_port *port) {
    if (!port) return DATAROUTER_ERR_INVALID_ARGUMENT;
    atomic_init(&port->rx_mutex.state, 0U);
    return DATAROUTER_OK;
}
static int port_rx_mutex_lock(datarouter_port *port) {
    if (!port) return DATAROUTER_ERR_INVALID_ARGUMENT;
    while (!datarouter_try_lock(&port->rx_mutex.state)) datarouter_cpu_pause();
    return DATAROUTER_OK;
}
static void port_rx_mutex_unlock(datarouter_port *port) {
    datarouter_unlock(&port->rx_mutex.state);
}
static void port_rx_mutex_destroy(datarouter_port *port) { (void)port; }
static int port_rx_mutex_trylock(datarouter_port *port) {
    if (!port) return DATAROUTER_ERR_INVALID_ARGUMENT;
    return datarouter_try_lock(&port->rx_mutex.state) ? DATAROUTER_OK : DATAROUTER_ERR_AGAIN;
}

static void encode_frame_header(
    uint8_t header[DATAROUTER_FRAME_HEADER_SIZE],
    uint32_t route_id,
    size_t len
) {
    uint32_t payload_len = (uint32_t)len;
    header[0] = (uint8_t)(route_id & 0xffU);
    header[1] = (uint8_t)((route_id >> 8U) & 0xffU);
    header[2] = (uint8_t)((route_id >> 16U) & 0xffU);
    header[3] = (uint8_t)((route_id >> 24U) & 0xffU);
    header[4] = (uint8_t)(payload_len & 0xffU);
    header[5] = (uint8_t)((payload_len >> 8U) & 0xffU);
    header[6] = (uint8_t)((payload_len >> 16U) & 0xffU);
    header[7] = (uint8_t)((payload_len >> 24U) & 0xffU);
}

static void decode_frame_header(
    const uint8_t header[DATAROUTER_FRAME_HEADER_SIZE],
    uint32_t *route_id,
    uint32_t *len
) {
    *route_id = (uint32_t)header[0] |
                ((uint32_t)header[1] << 8U) |
                ((uint32_t)header[2] << 16U) |
                ((uint32_t)header[3] << 24U);
    *len = (uint32_t)header[4] |
           ((uint32_t)header[5] << 8U) |
           ((uint32_t)header[6] << 16U) |
           ((uint32_t)header[7] << 24U);
}

static void report_corrupt_self(datarouter_port *port) {
    if (!port || !port->shm || port->slot_index >= datarouter_active_max_ports(port->shm)) {
        return;
    }
    datarouter_scheduler_mark_corrupt(port->shm, port->slot_index);
    datarouter_signal_post_if_waiting(&port->shm->control.switch_signal);
}

static int switch_identity_alive(datarouter_port *port, int force) {
    if (!port || !port->shm || atomic_load_explicit(&port->switch_dead, memory_order_acquire)) {
        return 0;
    }
    uint64_t now = datarouter_now_ns();
    uint64_t last = atomic_load_explicit(
        &port->last_switch_identity_check_ns,
        memory_order_acquire
    );
    if (!force && now >= last && now - last < DATAROUTER_SWITCH_IDENTITY_CACHE_NS) {
        return 1;
    }
    if (!datarouter_process_identity_matches(
            (pid_t)port->switch_pid,
            port->switch_uid,
            port->switch_start_id
        )) {
        atomic_store_explicit(&port->switch_dead, 1U, memory_order_release);
        return 0;
    }
    atomic_store_explicit(&port->last_switch_identity_check_ns, now, memory_order_release);
    return 1;
}

static datarouter_registry_entry *pin_self_entry(datarouter_port *port) {
    if (!port || !port->shm || port->slot_index >= datarouter_active_max_ports(port->shm)) {
        return NULL;
    }
    if (!switch_identity_alive(port, 0)) {
        return NULL;
    }
    datarouter_registry_entry *self = &port->shm->control.registry[port->slot_index];
    return datarouter_registry_pin_self(
               self,
               current_pid(),
               current_uid(),
               port->process_start_id,
               port->route_id,
               port->service_name
           )
               ? self
               : NULL;
}

static void touch_self(datarouter_port *port) {
    datarouter_registry_entry *self = pin_self_entry(port);
    datarouter_touch_entry(self, datarouter_now_ns());
    datarouter_registry_unpin(self);
}

/*
 * Advisory probe only: the caller owns rx_mutex. Writers publish head only
 * after copying a complete frame, and the local receiver owns tail, so this
 * single validate/peek snapshot is safe under the mutex. Malformed or partial
 * data is reported as AGAIN; authoritative receive paths perform full checks.
 */
static int channel_has_complete_frame(datarouter_channel rx) {
    if (!rx.header) {
        return DATAROUTER_ERR_INVALID_ARGUMENT;
    }

    size_t used = 0U;
    int rc = datarouter_ring_validate(rx, &used);
    if (rc == DATAROUTER_ERR_PROTOCOL) {
        return DATAROUTER_ERR_AGAIN;
    }
    if (rc != DATAROUTER_OK) {
        return rc;
    }
    if (used < DATAROUTER_FRAME_HEADER_SIZE) {
        return DATAROUTER_ERR_AGAIN;
    }

    uint8_t header[DATAROUTER_FRAME_HEADER_SIZE];
    rc = datarouter_ring_peek(rx, header, sizeof(header));
    if (rc == DATAROUTER_ERR_PROTOCOL) {
        return DATAROUTER_ERR_AGAIN;
    }
    if (rc != DATAROUTER_OK) {
        return rc;
    }

    uint32_t source_route = 0U;
    uint32_t payload_len = 0U;
    decode_frame_header(header, &source_route, &payload_len);
    if (source_route == 0U ||
        payload_len > (uint32_t)(DATAROUTER_RING_CAPACITY - DATAROUTER_FRAME_HEADER_SIZE)) {
        return DATAROUTER_ERR_AGAIN;
    }
    size_t frame_len = DATAROUTER_FRAME_HEADER_SIZE + (size_t)payload_len;
    return used >= frame_len ? DATAROUTER_OK : DATAROUTER_ERR_AGAIN;
}

/* Called only under rx_mutex. Keep a chosen source until its frame is
 * consumed, including poll/frame_len probes and short-buffer retries. */
static datarouter_channel select_rx(datarouter_port *port) {
    uint32_t n = datarouter_active_max_ports(port->shm);
    port->rx_cursor %= n;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t source = (port->rx_cursor + i) % n;
        datarouter_channel rx = datarouter_rx_channel(port->shm, source, port->slot_index);
        if (datarouter_ring_used(rx) != 0U) {
            port->rx_cursor = source;
            return rx;
        }
    }
    return datarouter_rx_channel(port->shm, port->rx_cursor, port->slot_index);
}

int datarouter_frame_len(datarouter_port *port, size_t *payload_len) {
    if (!port || !port->shm || !payload_len ||
        port->slot_index >= datarouter_active_max_ports(port->shm)) {
        return DATAROUTER_ERR_INVALID_ARGUMENT;
    }
    if (datarouter_thread_attach() < 0) return DATAROUTER_ERR_PLATFORM;
    datarouter_registry_entry *self = pin_self_entry(port);
    if (!self) {
        return DATAROUTER_ERR_NOT_FOUND;
    }
    int lock_rc = port_rx_mutex_lock(port);
    if (lock_rc != DATAROUTER_OK) {
        datarouter_registry_unpin(self);
        return lock_rc;
    }
    datarouter_touch_entry(self, datarouter_now_ns());
    datarouter_channel rx = select_rx(port);
    size_t used = 0U;
    int rc = datarouter_ring_validate(rx, &used);
    if (rc == DATAROUTER_OK) {
        if (used < DATAROUTER_FRAME_HEADER_SIZE) {
            rc = used == 0U ? DATAROUTER_ERR_AGAIN : DATAROUTER_ERR_PROTOCOL;
        } else {
            uint8_t header[DATAROUTER_FRAME_HEADER_SIZE];
            rc = datarouter_ring_peek(rx, header, sizeof(header));
            if (rc == DATAROUTER_OK) {
                uint32_t source_route = 0U;
                uint32_t len = 0U;
                decode_frame_header(header, &source_route, &len);
                if (source_route == 0U ||
                    len > (uint32_t)(DATAROUTER_RING_CAPACITY - DATAROUTER_FRAME_HEADER_SIZE) ||
                    used < DATAROUTER_FRAME_HEADER_SIZE + (size_t)len) {
                    rc = DATAROUTER_ERR_PROTOCOL;
                    report_corrupt_self(port);
                } else {
                    *payload_len = (size_t)len;
                }
            }
        }
    }
    if (rc == DATAROUTER_ERR_PROTOCOL) {
        report_corrupt_self(port);
    }
    port_rx_mutex_unlock(port);
    datarouter_registry_unpin(self);
    return rc;
}

static void fill_message_info(
    datarouter_port *port,
    uint32_t source_route,
    uint32_t payload_len,
    datarouter_message_info *info
) {
    memset(info, 0, sizeof(*info));
    info->len = (size_t)payload_len;
    if (port->cached_source_route == source_route && port->cached_source_name[0] != '\0') {
        snprintf(info->source, sizeof(info->source), "%s", port->cached_source_name);
        return;
    }
    datarouter_registry_metadata source;
    if (datarouter_registry_find_source_metadata_by_route(port->shm, source_route, &source)) {
        snprintf(info->source, sizeof(info->source), "%s", source.service_name);
        port->cached_source_route = source_route;
        snprintf(
            port->cached_source_name,
            sizeof(port->cached_source_name),
            "%s",
            source.service_name
        );
    }
}

static int recv_frame_unchecked(
    datarouter_port *port,
    datarouter_channel rx,
    void *buffer,
    size_t capacity,
    datarouter_message_info *info,
    int wake_blocked
) {
    size_t used = 0U;
    int rc = datarouter_ring_validate(rx, &used);
    if (rc != DATAROUTER_OK) {
        if (rc == DATAROUTER_ERR_PROTOCOL) {
            report_corrupt_self(port);
        }
        return rc;
    }
    if (used < DATAROUTER_FRAME_HEADER_SIZE) {
        if (used != 0U) {
            report_corrupt_self(port);
            return DATAROUTER_ERR_PROTOCOL;
        }
        return DATAROUTER_ERR_AGAIN;
    }

    uint8_t header[DATAROUTER_FRAME_HEADER_SIZE];
    rc = datarouter_ring_peek(rx, header, sizeof(header));
    if (rc != DATAROUTER_OK) {
        return rc;
    }
    uint32_t source_route = 0U;
    uint32_t payload_len = 0U;
    decode_frame_header(header, &source_route, &payload_len);
    fill_message_info(port, source_route, payload_len, info);
    if (source_route == 0U ||
        payload_len > (uint32_t)(DATAROUTER_RING_CAPACITY - DATAROUTER_FRAME_HEADER_SIZE)) {
        report_corrupt_self(port);
        return DATAROUTER_ERR_PROTOCOL;
    }
    size_t frame_len = DATAROUTER_FRAME_HEADER_SIZE + (size_t)payload_len;
    if (used < frame_len) {
        report_corrupt_self(port);
        return DATAROUTER_ERR_PROTOCOL;
    }
    if (capacity < (size_t)payload_len) {
        return DATAROUTER_ERR_PAYLOAD_TOO_LARGE;
    }

    rc = datarouter_ring_read_frame(rx, (uint8_t *)buffer, payload_len);
    if (rc != DATAROUTER_OK) {
        if (rc == DATAROUTER_ERR_PROTOCOL) {
            report_corrupt_self(port);
        }
        return rc;
    }
    port->rx_cursor = (port->rx_cursor + 1U) % datarouter_active_max_ports(port->shm);
    if (wake_blocked && datarouter_scheduler_wake_blocked(port->shm) > 0U) {
        datarouter_signal_post_if_waiting(&port->shm->control.switch_signal);
    }
    return DATAROUTER_OK;
}

static uint32_t normalize_drain_budget_us(uint32_t budget_us) {
    if (budget_us == 0U) {
        return DATAROUTER_PORT_DRAIN_BUDGET_US;
    }
    return budget_us > DATAROUTER_PORT_DRAIN_MAX_BUDGET_US
        ? DATAROUTER_PORT_DRAIN_MAX_BUDGET_US
        : budget_us;
}

static uint64_t next_request_nonce(datarouter_port *port) {
    uint64_t counter = atomic_fetch_add_explicit(&g_request_nonce, 1ULL, memory_order_relaxed);
    uint64_t nonce = datarouter_now_ns() ^ port->process_start_id ^
                     ((uint64_t)(uint32_t)current_pid() << 32U) ^ counter;
    return nonce == 0ULL ? counter + 1ULL : nonce;
}

static uint64_t request_claim_token(const datarouter_port *port) {
    uint32_t identity = (uint32_t)(port->process_start_id ^ (port->process_start_id >> 32U));
    if (identity == 0U) {
        identity = 1U;
    }
    return ((uint64_t)(uint32_t)current_pid() << 32U) | (uint64_t)identity;
}

static int request_owned_by(
    const datarouter_register_request *req,
    const datarouter_port *port,
    uint64_t nonce
) {
    return atomic_load_explicit(&req->claim_token, memory_order_acquire) ==
               request_claim_token(port) &&
           req->owner_pid == current_pid() && req->owner_uid == current_uid() &&
           req->owner_start_id == port->process_start_id && req->request_nonce == nonce;
}

static int submit_control_request(
    datarouter_port *port,
    datarouter_reg_operation operation,
    const char *service_name,
    uint32_t route_id,
    uint32_t timeout_ms,
    uint32_t *assigned_slot,
    uint32_t *assigned_route,
    char *errbuf,
    size_t errbuf_len
) {
    uint32_t actual_uid = 0U;
    uint64_t actual_start_id = 0ULL;
    int identity_rc = datarouter_process_identity(
        (pid_t)current_pid(),
        &actual_uid,
        &actual_start_id
    );
    if (identity_rc != DATAROUTER_OK || actual_uid != current_uid() ||
        actual_start_id != port->process_start_id) {
        datarouter_set_error(
            errbuf,
            errbuf_len,
            "port handle belongs to a different process; reopen after fork"
        );
        return identity_rc == DATAROUTER_OK ? DATAROUTER_ERR_NOT_FOUND : identity_rc;
    }
    datarouter_register_request *req = NULL;
    uint32_t claimed_slot = UINT32_MAX;
    uint64_t started = datarouter_now_ns();
    uint64_t timeout_ns = (uint64_t)timeout_ms * 1000000ULL;
    uint64_t deadline = started + timeout_ns;
    if (deadline < started) {
        deadline = UINT64_MAX;
    }
    uint64_t nonce = next_request_nonce(port);
    unsigned long long claim_token = request_claim_token(port);

    for (;;) {
        for (uint32_t request_slot = 0U;
             request_slot < DATAROUTER_REGISTRATION_REQUEST_SLOTS;
             request_slot++) {
            datarouter_register_request *candidate =
                &port->shm->control.register_requests[request_slot];
            unsigned long long empty_claim = 0ULL;
            if (atomic_compare_exchange_strong_explicit(
                    &candidate->claim_token,
                    &empty_claim,
                    claim_token,
                    memory_order_acq_rel,
                    memory_order_acquire
                )) {
                req = candidate;
                claimed_slot = request_slot;
                break;
            }
        }
        if (req) {
            req->operation = (uint32_t)operation;
            req->owner_pid = current_pid();
            req->owner_uid = current_uid();
            req->owner_start_id = port->process_start_id;
            req->request_nonce = nonce;
            req->response_nonce = 0ULL;
            req->deadline_ns = deadline;
            snprintf(req->service_name, sizeof(req->service_name), "%s", service_name);
            req->route_id = route_id;
            req->assigned_slot = UINT32_MAX;
            req->assigned_route_id = 0U;
            req->result_code = DATAROUTER_ERR_AGAIN;
            memset(req->message, 0, sizeof(req->message));
            if (atomic_load_explicit(&req->claim_token, memory_order_acquire) != claim_token) {
                datarouter_set_error(errbuf, errbuf_len, "control request claim was recovered");
                return DATAROUTER_ERR_TIMEOUT;
            }
            unsigned int empty_state = DATAROUTER_REG_EMPTY;
            if (!atomic_compare_exchange_strong_explicit(
                    &req->state,
                    &empty_state,
                    DATAROUTER_REG_PENDING,
                    memory_order_release,
                    memory_order_acquire
                )) {
                unsigned long long owned_claim = claim_token;
                (void)atomic_compare_exchange_strong_explicit(
                    &req->claim_token,
                    &owned_claim,
                    0ULL,
                    memory_order_acq_rel,
                    memory_order_acquire
                );
                datarouter_set_error(errbuf, errbuf_len, "control request state is inconsistent");
                return DATAROUTER_ERR_PROTOCOL;
            }
            atomic_fetch_or_explicit(
                &port->shm->control.request_ready_mask,
                1ULL << claimed_slot,
                memory_order_release
            );
            datarouter_signal_post_if_waiting(&port->shm->control.switch_signal);
            break;
        }
        if (datarouter_now_ns() >= deadline) {
            datarouter_set_error(errbuf, errbuf_len, "control request slot is busy");
            return DATAROUTER_ERR_TIMEOUT;
        }
        datarouter_yield();
    }

    for (;;) {
        uint32_t signal_sequence = datarouter_signal_prepare(&port->shm->control.registration_signal);
        unsigned int state = atomic_load_explicit(&req->state, memory_order_acquire);
        if (state == DATAROUTER_REG_ACCEPTED || state == DATAROUTER_REG_REJECTED) {
            unsigned int terminal = state;
            if (!atomic_compare_exchange_strong_explicit(
                    &req->state,
                    &terminal,
                    DATAROUTER_REG_CONSUMING,
                    memory_order_acq_rel,
                    memory_order_acquire
                )) {
                continue;
            }
            if (!request_owned_by(req, port, nonce)) {
                atomic_store_explicit(&req->state, DATAROUTER_REG_EMPTY, memory_order_release);
                unsigned long long owned_claim = claim_token;
                (void)atomic_compare_exchange_strong_explicit(
                    &req->claim_token,
                    &owned_claim,
                    0ULL,
                    memory_order_acq_rel,
                    memory_order_acquire
                );
                datarouter_set_error(errbuf, errbuf_len, "control response owner mismatch");
                return DATAROUTER_ERR_PROTOCOL;
            }
            if (req->response_nonce != nonce) {
                atomic_store_explicit(&req->state, DATAROUTER_REG_EMPTY, memory_order_release);
                atomic_store_explicit(&req->claim_token, 0ULL, memory_order_release);
                datarouter_set_error(errbuf, errbuf_len, "control response nonce mismatch");
                return DATAROUTER_ERR_PROTOCOL;
            }
            int result = req->result_code;
            if (assigned_slot) {
                *assigned_slot = req->assigned_slot;
            }
            if (assigned_route) {
                *assigned_route = req->assigned_route_id;
            }
            datarouter_set_error(errbuf, errbuf_len, req->message);
            atomic_store_explicit(&req->state, DATAROUTER_REG_EMPTY, memory_order_release);
            atomic_store_explicit(&req->claim_token, 0ULL, memory_order_release);
            if (state == DATAROUTER_REG_REJECTED && result == DATAROUTER_OK) {
                return DATAROUTER_ERR_INTERNAL;
            }
            return result;
        }
        if (!request_owned_by(req, port, nonce)) {
            datarouter_set_error(errbuf, errbuf_len, "control request was recovered by switch");
            return datarouter_now_ns() >= deadline
                ? DATAROUTER_ERR_TIMEOUT
                : DATAROUTER_ERR_PROTOCOL;
        }

        uint64_t now = datarouter_now_ns();
        if (now >= deadline) {
            if (state == DATAROUTER_REG_PENDING && request_owned_by(req, port, nonce)) {
                unsigned int pending = DATAROUTER_REG_PENDING;
                (void)atomic_compare_exchange_strong_explicit(
                    &req->state,
                    &pending,
                    DATAROUTER_REG_EMPTY,
                    memory_order_acq_rel,
                    memory_order_acquire
                );
                if (pending == DATAROUTER_REG_PENDING) {
                    unsigned long long owned_claim = claim_token;
                    (void)atomic_compare_exchange_strong_explicit(
                        &req->claim_token,
                        &owned_claim,
                        0ULL,
                        memory_order_acq_rel,
                        memory_order_acquire
                    );
                    datarouter_set_error(
                        errbuf,
                        errbuf_len,
                        "switch did not answer control request in time"
                    );
                    return DATAROUTER_ERR_TIMEOUT;
                }
            }
            if (state == DATAROUTER_REG_PROCESSING && request_owned_by(req, port, nonce)) {
                unsigned int processing = DATAROUTER_REG_PROCESSING;
                if (atomic_compare_exchange_strong_explicit(
                        &req->state,
                        &processing,
                        DATAROUTER_REG_ABANDONED,
                        memory_order_acq_rel,
                        memory_order_acquire
                    )) {
                    datarouter_set_error(
                        errbuf,
                        errbuf_len,
                        "switch did not answer control request in time"
                    );
                    return DATAROUTER_ERR_TIMEOUT;
                }
            }
            /* The switch may have published a response while the deadline
             * branch raced its PROCESSING state. Loop once more so that the
             * terminal response is consumed instead of leaking the mailbox. */
            continue;
        }
        uint64_t remaining_ns = deadline - now;
        uint32_t wait_ms = (uint32_t)((remaining_ns + 999999ULL) / 1000000ULL);
        if (wait_ms > DATAROUTER_SWITCH_WAIT_MS) {
            wait_ms = DATAROUTER_SWITCH_WAIT_MS;
        }
        if (wait_ms == 0U) {
            wait_ms = 1U;
        }
        int wait_rc = datarouter_signal_wait_prepared(&port->shm->control.registration_signal, signal_sequence, wait_ms);
        if (wait_rc != DATAROUTER_OK && wait_rc != DATAROUTER_ERR_TIMEOUT) {
            return wait_rc;
        }
    }
}

int datarouter_open(const datarouter_options *options, datarouter_port **out) {
    if (!out) {
        return DATAROUTER_ERR_INVALID_ARGUMENT;
    }
    *out = NULL;
    const char *shm_name = option_shm_name(options);
    if (!datarouter_valid_shm_name(shm_name)) {
        return DATAROUTER_ERR_INVALID_ARGUMENT;
    }

    datarouter_mapping mapping;
    int rc = datarouter_platform_open(shm_name, 0U, &mapping);
    if (rc != DATAROUTER_OK) {
        return rc;
    }
    rc = datarouter_validate_shared_memory(mapping.shm, mapping.map_size);
    if (rc != DATAROUTER_OK) {
        datarouter_platform_close(&mapping);
        return rc;
    }
    if (!datarouter_process_identity_matches(
            (pid_t)mapping.shm->control.switch_pid,
            mapping.shm->control.switch_uid,
            mapping.shm->control.switch_start_id
        )) {
        datarouter_platform_close(&mapping);
        return DATAROUTER_ERR_NOT_FOUND;
    }
    uint64_t expected_fingerprint = mapping.shm->control.auth_fingerprint;
    const char *token = options && options->auth_token && options->auth_token[0] != '\0'
        ? options->auth_token
        : NULL;
    if (expected_fingerprint != 0ULL &&
        datarouter_auth_fingerprint(token) != expected_fingerprint) {
        datarouter_platform_close(&mapping);
        return DATAROUTER_ERR_PERMISSION;
    }

    datarouter_port *port = (datarouter_port *)calloc(1U, sizeof(*port));
    if (!port) {
        datarouter_platform_close(&mapping);
        return DATAROUTER_ERR_INTERNAL;
    }
    port->fd = mapping.fd;
    port->map_size = mapping.map_size;
    port->shm = mapping.shm;
    port->platform_handle = mapping.platform_handle;
    port->slot_index = UINT32_MAX;
    port->timeout_ms = option_timeout_ms(options);
    port->switch_pid = mapping.shm->control.switch_pid;
    port->switch_uid = mapping.shm->control.switch_uid;
    port->switch_start_id = mapping.shm->control.switch_start_id;
    atomic_init(&port->last_switch_identity_check_ns, datarouter_now_ns());
    atomic_init(&port->switch_dead, 0U);
    uint32_t identity_uid = 0U;
    rc = datarouter_process_identity((pid_t)current_pid(), &identity_uid, &port->process_start_id);
    if (rc != DATAROUTER_OK || identity_uid != current_uid()) {
        datarouter_platform_close(&mapping);
        free(port);
        return rc == DATAROUTER_OK ? DATAROUTER_ERR_PERMISSION : rc;
    }
    rc = port_tx_mutex_init(port);
    if (rc != DATAROUTER_OK) {
        datarouter_platform_close(&mapping);
        free(port);
        return rc;
    }
    rc = port_rx_mutex_init(port);
    if (rc != DATAROUTER_OK) {
        port_tx_mutex_destroy(port);
        datarouter_platform_close(&mapping);
        free(port);
        return rc;
    }
    *out = port;
    return DATAROUTER_OK;
}

int datarouter_logon(
    datarouter_port *port,
    const char *service_name,
    char *errbuf,
    size_t errbuf_len
) {
    if (!port || !port->shm || !datarouter_valid_service_name(service_name)) {
        datarouter_set_error(errbuf, errbuf_len, "invalid service name");
        return DATAROUTER_ERR_INVALID_ARGUMENT;
    }
    if (!switch_identity_alive(port, 1)) {
        datarouter_set_error(errbuf, errbuf_len, "datarouter switch is not running");
        return DATAROUTER_ERR_NOT_FOUND;
    }
    if (port->route_id != 0U) {
        datarouter_set_error(errbuf, errbuf_len, "port is already logged on");
        return DATAROUTER_ERR_BUSY;
    }
    uint32_t slot = UINT32_MAX;
    uint32_t route = 0U;
    int rc = submit_control_request(
        port,
        DATAROUTER_REG_OP_REGISTER,
        service_name,
        0U,
        port->timeout_ms,
        &slot,
        &route,
        errbuf,
        errbuf_len
    );
    if (rc == DATAROUTER_OK) {
        if (slot >= datarouter_active_max_ports(port->shm) || route == 0U) {
            return DATAROUTER_ERR_PROTOCOL;
        }
        port->slot_index = slot;
        port->route_id = route;
        snprintf(port->service_name, sizeof(port->service_name), "%s", service_name);
    }
    return rc;
}

int DR_IO_NAME(datarouter_send)(
    datarouter_port *port,
    const char *target_service,
    const void *data,
    size_t len
) {
    if (!port || !port->shm || port->slot_index >= datarouter_active_max_ports(port->shm) ||
        !datarouter_valid_service_name(target_service) || (!data && len > 0U)) {
        return DATAROUTER_ERR_INVALID_ARGUMENT;
    }
    if (len > UINT32_MAX ||
        len > DATAROUTER_RING_CAPACITY - DATAROUTER_FRAME_HEADER_SIZE) {
        return DATAROUTER_ERR_PAYLOAD_TOO_LARGE;
    }
    if (datarouter_thread_attach() < 0) return DATAROUTER_ERR_PLATFORM;
    int lock_rc = port_tx_mutex_lock(port);
    if (lock_rc != DATAROUTER_OK) {
        return lock_rc;
    }
    int rc = DATAROUTER_OK;
    datarouter_registry_entry *self = pin_self_entry(port);
    if (!self) {
        rc = DATAROUTER_ERR_NOT_FOUND;
        goto out;
    }
    datarouter_registry_metadata target;
    if (!datarouter_registry_find_active_metadata_by_name(port->shm, target_service, &target)) {
        rc = DATAROUTER_ERR_NOT_FOUND;
        goto out;
    }
    datarouter_channel tx = datarouter_tx_channel(port->shm, port->slot_index);
    if (!tx.header) {
        rc = DATAROUTER_ERR_INTERNAL;
        goto out;
    }
    uint8_t header[DATAROUTER_FRAME_HEADER_SIZE];
    encode_frame_header(header, target.route_id, len);
    rc = datarouter_ring_write_parts(tx, header, sizeof(header), (const uint8_t *)data, len);
    if (rc == DATAROUTER_ERR_PROTOCOL) {
        report_corrupt_self(port);
        goto out;
    }
    if (rc != DATAROUTER_OK) {
        goto out;
    }
    datarouter_scheduler_mark_tx_ready(port->shm, port->slot_index);
    datarouter_signal_post_if_waiting(&port->shm->control.switch_signal);
    datarouter_touch_entry(self, datarouter_now_ns());

out:
    datarouter_registry_unpin(self);
    port_tx_mutex_unlock(port);
    return rc;
}

#ifdef DATAROUTER_UNSAFE

/* Direct (unsafe) data plane: bypass the switch for payload movement.
 *
 * The sender resolves the target from the shared-memory registry, pins the
 * target slot via the direct-writer pin counter (blocking release/reuse while
 * the write is in flight), tracks the inflight counter exactly like the
 * switch's router would, then appends the frame header + payload straight
 * into the target's RX ring and posts the target's rx_signal directly. The
 * switch never touches the payload bytes; it only keeps control-plane duties
 * (register/unregister, TTL/dead-process reaping) and remains the
 * control-request interrupt target.
 *
 * The frame format is identical to the switch-routed path, so a receiver may
 * use either the normal `datarouter_recv`/`datarouter_wait` or the direct
 * Frontend API; both complete the inflight counter the same way.
 *
 * Constraints (dev feature):
 *  - concurrent direct and routed writers share nonblocking RX ownership;
 *    contention returns DATAROUTER_ERR_AGAIN,
 *  - a full target RX ring returns DATAROUTER_ERR_FULL (no switch blocked
 *    queue/requeue protocol in the direct path),
 *  - the target is re-validated against the registry while pinned, so slot
 *    release/reuse cannot race the write; a mid-write unregister may still
 *    drop the frame when the slot is finalized.
 */
int DR_IO_NAME(datarouter_unsafe_send)(
    datarouter_port *port,
    const char *target_service,
    const void *data,
    size_t len
) {
    if (!port || !port->shm || port->slot_index >= datarouter_active_max_ports(port->shm) ||
        !datarouter_valid_service_name(target_service) || (!data && len > 0U)) {
        return DATAROUTER_ERR_INVALID_ARGUMENT;
    }
    if (len > UINT32_MAX ||
        len > DATAROUTER_RING_CAPACITY - DATAROUTER_FRAME_HEADER_SIZE) {
        return DATAROUTER_ERR_PAYLOAD_TOO_LARGE;
    }
    if (datarouter_thread_attach() < 0) return DATAROUTER_ERR_PLATFORM;
    int lock_rc = port_tx_mutex_lock(port);
    if (lock_rc != DATAROUTER_OK) {
        return lock_rc;
    }
    int rc = DATAROUTER_OK;
    datarouter_registry_entry *self = pin_self_entry(port);
    if (!self) {
        rc = DATAROUTER_ERR_NOT_FOUND;
        goto out;
    }
    datarouter_registry_metadata target;
    if (!datarouter_registry_find_active_metadata_by_name(
            port->shm,
            target_service,
            &target
        )) {
        rc = DATAROUTER_ERR_NOT_FOUND;
        goto out;
    }
    uint32_t target_slot = target.slot_index;
    if (target_slot >= datarouter_active_max_ports(port->shm)) {
        rc = DATAROUTER_ERR_NOT_FOUND;
        goto out;
    }
    datarouter_registry_entry *target_entry = &port->shm->control.registry[target_slot];
    /* Acquire the same RX writer ownership as routed sends. Publish the PID
     * atomically with ownership so teardown never mistakes a paused claimant
     * for a dead writer during identity publication. */
    datarouter_channel rx = datarouter_rx_channel(port->shm, port->slot_index, target_slot);
    unsigned int expected_writer = 0;
    /* pin_self_entry already checked the calling PID/UID and holds this
     * registration stable until out. Legacy direct_writer_* fields are no
     * longer read by recovery, so publishing them only adds stores and
     * redundant getpid/getuid calls to every frame. */
    unsigned int writer_pid = (unsigned int)atomic_load_explicit(&self->pid, memory_order_relaxed);
    if (!atomic_compare_exchange_strong_explicit(rx.writer_owner,
            &expected_writer, writer_pid, memory_order_seq_cst, memory_order_relaxed)) {
        rc = DATAROUTER_ERR_AGAIN;
        goto out;
    }
    /* Re-verify the pinned entry is still the same active registration. */
    datarouter_registry_metadata recheck;
    if (!datarouter_registry_read_active(target_entry, &recheck) ||
        recheck.route_id != target.route_id || recheck.slot_index != target_slot) {
        atomic_store_explicit(rx.writer_owner, 0U, memory_order_seq_cst);
        rc = DATAROUTER_ERR_NOT_FOUND;
        goto out;
    }
    if (!rx.header) {
        atomic_store_explicit(rx.writer_owner, 0U, memory_order_seq_cst);
        rc = DATAROUTER_ERR_INTERNAL;
        goto out;
    }
    uint8_t header[DATAROUTER_FRAME_HEADER_SIZE];
    encode_frame_header(header, port->route_id, len);
    rc = datarouter_ring_write_parts(rx, header, sizeof(header), (const uint8_t *)data, len);
    if (rc != DATAROUTER_OK) {
        atomic_store_explicit(rx.writer_owner, 0U, memory_order_seq_cst);
        if (rc == DATAROUTER_ERR_PROTOCOL) {
            report_corrupt_self(port);
        }
        goto out;
    }
    atomic_store_explicit(rx.writer_owner, 0U, memory_order_seq_cst);
    datarouter_signal_post_if_waiting(&port->shm->control.rx_signal[target_slot]);
    datarouter_touch_entry(self, datarouter_now_ns());

out:
    datarouter_registry_unpin(self);
    port_tx_mutex_unlock(port);
    return rc;
}

#endif /* DATAROUTER_UNSAFE */

int DR_IO_NAME(datarouter_recv)(
    datarouter_port *port,
    void *buffer,
    size_t capacity,
    datarouter_message_info *info
) {
    if (!port || !port->shm || port->slot_index >= datarouter_active_max_ports(port->shm) ||
        (!buffer && capacity > 0U) || !info) {
        return DATAROUTER_ERR_INVALID_ARGUMENT;
    }
    if (datarouter_thread_attach() < 0) return DATAROUTER_ERR_PLATFORM;
    datarouter_registry_entry *self = pin_self_entry(port);
    if (!self) {
        return DATAROUTER_ERR_NOT_FOUND;
    }
    datarouter_touch_entry(self, datarouter_now_ns());
    /* The direct (unsafe) data plane has no reader thread; multiple
     * application threads may call datarouter_recv concurrently on the same
     * SPSC RX ring. Serialize ring reads within this process. */
    int rx_lock_rc = port_rx_mutex_lock(port);
    if (rx_lock_rc != DATAROUTER_OK) {
        datarouter_registry_unpin(self);
        return rx_lock_rc;
    }
    datarouter_channel rx = select_rx(port);
    int rc = recv_frame_unchecked(port, rx, buffer, capacity, info, 1);
    port_rx_mutex_unlock(port);
    datarouter_registry_unpin(self);
    return rc;
}

int datarouter_poll(datarouter_port *port) {
    if (!port || !port->shm || port->slot_index >= datarouter_active_max_ports(port->shm)) {
        return DATAROUTER_ERR_INVALID_ARGUMENT;
    }
    if (datarouter_thread_attach() < 0) return DATAROUTER_ERR_PLATFORM;
    datarouter_registry_entry *self = pin_self_entry(port);
    if (!self) {
        return DATAROUTER_ERR_NOT_FOUND;
    }
    datarouter_touch_entry(self, datarouter_now_ns());
    /* Poll is a non-authoritative snapshot. A concurrent authoritative
     * consumer owns rx_mutex, so a busy trylock reports ERR_AGAIN. */
    int rc = port_rx_mutex_trylock(port);
#ifdef DATAROUTER_WAIT_DIAGNOSTICS
    dr_poll_busy = rc == DATAROUTER_ERR_AGAIN;
#endif
    if (rc == DATAROUTER_OK) {
        rc = channel_has_complete_frame(select_rx(port));
        port_rx_mutex_unlock(port);
    }
    datarouter_registry_unpin(self);
    return rc;
}

int datarouter_touch(datarouter_port *port) {
    if (!port || !port->shm || port->slot_index >= datarouter_active_max_ports(port->shm)) {
        return DATAROUTER_ERR_INVALID_ARGUMENT;
    }
    if (!switch_identity_alive(port, 1)) {
        return DATAROUTER_ERR_NOT_FOUND;
    }
    datarouter_registry_entry *self = pin_self_entry(port);
    if (!self) {
        return DATAROUTER_ERR_NOT_FOUND;
    }
    datarouter_touch_entry(self, datarouter_now_ns());
    datarouter_registry_unpin(self);
    return DATAROUTER_OK;
}

int datarouter_wait(datarouter_port *port, uint32_t timeout_ms) {
    if (!port || !port->shm || port->slot_index >= datarouter_active_max_ports(port->shm)) {
        return DATAROUTER_ERR_INVALID_ARGUMENT;
    }
    int dedicated = datarouter_thread_attach();
    if (dedicated < 0) return DATAROUTER_ERR_PLATFORM;
    uint64_t started = datarouter_now_ns();
    DR_WAIT_BEGIN(started);
    uint64_t timeout_ns = (uint64_t)timeout_ms * 1000000ULL;
    /* One cumulative budget per invocation, never replenished by a wake. */
    uint64_t spin_remaining = (uint64_t)DATAROUTER_PORT_SPIN_BUDGET_US * 1000ULL;
    int wake_rc = DATAROUTER_OK;
    for (;;) {
        uint64_t spin_started = datarouter_now_ns();
        /* A timed wake keeps poll's forced identity check. All wake paths
         * share one registration reference for heartbeat and advisory probe. */
        if (wake_rc != DATAROUTER_OK && !switch_identity_alive(port, 1)) {
            DR_WAIT_RETURN(DATAROUTER_ERR_NOT_FOUND);
        }
        datarouter_registry_entry *self = pin_self_entry(port);
        if (!self) { DR_WAIT_RETURN(DATAROUTER_ERR_NOT_FOUND); }
        datarouter_touch_entry(self, spin_started);
        for (;;) {
            /* The pin prevents reuse. Check revocation on every probe under
             * the same RX mutex used by authoritative receivers. */
            int rc = port_rx_mutex_trylock(port);
#ifdef DATAROUTER_WAIT_DIAGNOSTICS
            dr_poll_busy = rc == DATAROUTER_ERR_AGAIN;
#endif
            if (rc == DATAROUTER_OK) {
                if (atomic_load_explicit(&self->state, memory_order_acquire) != DATAROUTER_PORT_ACTIVE ||
                    (atomic_load_explicit(&self->io_users, memory_order_acquire) & DATAROUTER_IO_CLOSED)) {
                    rc = DATAROUTER_ERR_NOT_FOUND;
                } else {
                    rc = channel_has_complete_frame(select_rx(port));
                }
                port_rx_mutex_unlock(port);
            }
            DR_WAIT_POLL(rc);
            DR_WAIT_SPIN();
            if (rc != DATAROUTER_ERR_AGAIN) {
                datarouter_registry_unpin(self);
                DR_WAIT_RETURN(rc);
            }
            uint64_t now = datarouter_now_ns();
            datarouter_cpu_pause();
            /* Dedicated polling periodically drops its pin for heartbeat and
             * switch-death checks; it never enters a kernel wait. */
            if ((dedicated && (now - spin_started >= 1000000ULL ||
                 (timeout_ns && now - started >= timeout_ns))) ||
                (!dedicated && !dr_wait_spin(spin_started, now, spin_remaining, started, timeout_ns))) {
                uint64_t spent = now - spin_started;
                spin_remaining = spent >= spin_remaining ? 0ULL : spin_remaining - spent;
                break;
            }
        }
        /* Never retain a registration reference while sleeping. */
        datarouter_registry_unpin(self);
        uint32_t wait_ms = 100U;
        if (timeout_ms > 0U) {
            uint64_t elapsed = datarouter_now_ns() - started;
            if (elapsed >= timeout_ns) {
                DR_WAIT_RETURN(DATAROUTER_ERR_TIMEOUT);
            }
            wait_ms = (uint32_t)((timeout_ns - elapsed + 999999ULL) / 1000000ULL);
            if (wait_ms > 100U) {
                wait_ms = 100U;
            }
        }
        if (dedicated) {
            if (!switch_identity_alive(port, 0)) DR_WAIT_RETURN(DATAROUTER_ERR_NOT_FOUND);
            continue;
        }
        datarouter_signal *signal = &port->shm->control.rx_signal[port->slot_index];
        int arm_rc = datarouter_signal_arm(signal);
        if (arm_rc != DATAROUTER_OK) DR_WAIT_RETURN(arm_rc);
        uint32_t signal_sequence = datarouter_signal_prepare(signal);
        int probe = datarouter_poll(port);
        if (probe != DATAROUTER_ERR_AGAIN) {
            datarouter_signal_cancel(signal);
            DR_WAIT_RETURN(probe);
        }
        int rc = datarouter_signal_wait_armed(
            signal,
            signal_sequence,
            wait_ms
        );
        DR_WAIT_KERNEL(rc);
        if (rc != DATAROUTER_OK && rc != DATAROUTER_ERR_TIMEOUT && rc != DATAROUTER_ERR_AGAIN) {
            if (!switch_identity_alive(port, 0)) DR_WAIT_RETURN(DATAROUTER_ERR_NOT_FOUND);
            touch_self(port);
            DR_WAIT_RETURN(rc);
        }
        /* Probe before checking the deadline, including after timeout: a
         * frame published at the sleep boundary must still be observable. */
        wake_rc = rc;
    }
}

int datarouter_drain(
    datarouter_port *port,
    uint32_t budget_us,
    size_t max_frames,
    datarouter_drain_callback callback,
    void *user_data,
    size_t *drained
) {
    if (!port || !port->shm || port->slot_index >= datarouter_active_max_ports(port->shm) ||
        !callback || !drained) {
        return DATAROUTER_ERR_INVALID_ARGUMENT;
    }
    *drained = 0U;
    datarouter_registry_entry *self = pin_self_entry(port);
    if (!self) {
        return DATAROUTER_ERR_NOT_FOUND;
    }
    if (max_frames == 0U) {
        datarouter_registry_unpin(self);
        return DATAROUTER_OK;
    }
    if (datarouter_thread_attach() < 0) { datarouter_registry_unpin(self); return DATAROUTER_ERR_PLATFORM; }
    size_t buffer_capacity = DATAROUTER_MAX_APP_PAYLOAD;
    uint8_t *buffer = malloc(buffer_capacity);
    if (!buffer) { datarouter_registry_unpin(self); return DATAROUTER_ERR_INTERNAL; }
    datarouter_touch_entry(self, datarouter_now_ns());
    uint64_t started = datarouter_now_ns();
    uint64_t budget_ns = (uint64_t)normalize_drain_budget_us(budget_us) * 1000ULL;
    int last_rc = DATAROUTER_ERR_AGAIN;
    while (*drained < max_frames) {
        int lock_rc = port_rx_mutex_lock(port);
        if (lock_rc != DATAROUTER_OK) { last_rc = lock_rc; break; }
        datarouter_message_info info;
        int rc = recv_frame_unchecked(
            port,
            select_rx(port),
            buffer,
            buffer_capacity,
            &info,
            1
        );
        port_rx_mutex_unlock(port);
        if (rc != DATAROUTER_OK) {
            last_rc = rc;
            if (rc == DATAROUTER_ERR_AGAIN && *drained == 0U &&
                datarouter_now_ns() - started < budget_ns) {
                continue;
            }
            break;
        }
        rc = callback(&info, buffer, info.len, user_data);
        (*drained)++;
        last_rc = rc;
        if (rc != DATAROUTER_OK || datarouter_now_ns() - started >= budget_ns) {
            break;
        }
    }
    if (*drained > 0U && datarouter_scheduler_wake_blocked(port->shm) > 0U) {
        datarouter_signal_post_if_waiting(&port->shm->control.switch_signal);
    }
    free(buffer);
    datarouter_registry_unpin(self);
    return *drained > 0U && last_rc == DATAROUTER_ERR_AGAIN ? DATAROUTER_OK : last_rc;
}

int datarouter_registry_snapshot(
    datarouter_port *port,
    datarouter_registry_snapshot_entry *entries,
    size_t capacity,
    size_t *count
) {
    if (!port || !port->shm || !count || (!entries && capacity > 0U)) {
        return DATAROUTER_ERR_INVALID_ARGUMENT;
    }
    if (!switch_identity_alive(port, 0)) {
        return DATAROUTER_ERR_NOT_FOUND;
    }
    size_t seen = 0U;
    uint32_t max_ports = datarouter_active_max_ports(port->shm);
    for (uint32_t i = 0U; i < max_ports; i++) {
        datarouter_registry_metadata metadata;
        if (!datarouter_registry_read_active(&port->shm->control.registry[i], &metadata)) {
            continue;
        }
        if (seen < capacity) {
            datarouter_registry_snapshot_entry *out = &entries[seen];
            memset(out, 0, sizeof(*out));
            snprintf(out->service_name, sizeof(out->service_name), "%s", metadata.service_name);
            out->pid = metadata.pid;
            out->route_id = metadata.route_id;
            out->channel_id = DATAROUTER_RX_CHANNEL(datarouter_active_max_ports(port->shm), 0U, metadata.slot_index);
            out->slot = metadata.slot_index;
            out->last_seen_ns = metadata.last_active_ns;
        }
        seen++;
    }
    *count = seen;
    return seen > capacity ? DATAROUTER_ERR_FULL : DATAROUTER_OK;
}

uint32_t datarouter_ttl_ms(datarouter_port *port) {
    return port && port->shm && switch_identity_alive(port, 0)
        ? port->shm->control.ttl_ms
        : 0U;
}

uint32_t datarouter_max_ports(datarouter_port *port) {
    return port && port->shm ? datarouter_active_max_ports(port->shm) : 0U;
}

uint32_t datarouter_active_ports(datarouter_port *port) {
    if (!port || !port->shm) {
        return 0U;
    }
    uint32_t active = 0U;
    uint32_t max_ports = datarouter_active_max_ports(port->shm);
    for (uint32_t i = 0U; i < max_ports; i++) {
        if (atomic_load_explicit(&port->shm->control.registry[i].state, memory_order_acquire) ==
            DATAROUTER_PORT_ACTIVE) {
            active++;
        }
    }
    return active;
}

int datarouter_transport_stats_snapshot(
    datarouter_port *port,
    datarouter_transport_stats *out
) {
    if (!port || !port->shm || !out) {
        return DATAROUTER_ERR_INVALID_ARGUMENT;
    }
    if (!switch_identity_alive(port, 0)) {
        return DATAROUTER_ERR_NOT_FOUND;
    }
    out->corrupted_rings = atomic_load_explicit(
        &port->shm->control.corrupted_rings,
        memory_order_relaxed
    );
    out->registration_recoveries = atomic_load_explicit(
        &port->shm->control.registration_recoveries,
        memory_order_relaxed
    );
    out->blocked_wakeups = atomic_load_explicit(
        &port->shm->control.blocked_wakeups,
        memory_order_relaxed
    );
    out->registration_request_recoveries = atomic_load_explicit(
        &port->shm->control.registration_request_recoveries,
        memory_order_relaxed
    );
    out->dead_process_reaps = atomic_load_explicit(
        &port->shm->control.dead_process_reaps,
        memory_order_relaxed
    );
    out->transport_generation = atomic_load_explicit(
        &port->shm->control.transport_generation,
        memory_order_relaxed
    );
    return DATAROUTER_OK;
}

static int unregister_self(datarouter_port *port, uint32_t timeout_ms) {
    datarouter_registry_entry *self = pin_self_entry(port);
    if (!self) {
        return DATAROUTER_ERR_NOT_FOUND;
    }
    datarouter_registry_unpin(self);
    char errbuf[96];
    return submit_control_request(
        port,
        DATAROUTER_REG_OP_UNREGISTER,
        port->service_name,
        port->route_id,
        timeout_ms,
        NULL,
        NULL,
        errbuf,
        sizeof(errbuf)
    );
}

void datarouter_close(datarouter_port *port) {
    if (!port) {
        return;
    }
    uint64_t keep_seq = atomic_load_explicit(&port->keep_ref_seq, memory_order_acquire);
    if (keep_seq) (void)datarouter_keep_verify_ref(port, keep_seq);
    if (port->shm && port->route_id != 0U) {
        (void)unregister_self(port, DATAROUTER_UNREGISTER_TIMEOUT_MS);
    }
    datarouter_mapping mapping = {
        .fd = port->fd,
        .map_size = port->map_size,
        .shm = port->shm,
        .owner = 0,
        .platform_handle = port->platform_handle,
    };
    datarouter_platform_close(&mapping);
    port_tx_mutex_destroy(port);
    port_rx_mutex_destroy(port);
    free(port);
#ifdef DATAROUTER_WAIT_DIAGNOSTICS
    /* Go exits without running C atexit/destructors; close is the ownership boundary. */
    dr_wait_dump();
    dr_io_dump();
#ifndef _WIN32
    datarouter_diagnostic_flush();
#endif
#endif
}

int datarouter_test_release_self_slot(datarouter_port *port) {
    if (!port || !port->shm || port->route_id == 0U) {
        return DATAROUTER_ERR_INVALID_ARGUMENT;
    }
    return unregister_self(port, port->timeout_ms);
}

int datarouter_keep_grant(datarouter_port *port, const char *service_name) {
    if (!port || !port->shm || port->route_id == 0U) {
        return DATAROUTER_ERR_INVALID_ARGUMENT;
    }
    if (!datarouter_valid_service_name(service_name)) {
        return DATAROUTER_ERR_INVALID_ARGUMENT;
    }
    uint32_t assigned_slot = 0U;
    char errbuf[128];
    return submit_control_request(
        port,
        DATAROUTER_REG_OP_KEEP_GRANT,
        service_name,
        port->route_id,
        port->timeout_ms,
        &assigned_slot,
        NULL,
        errbuf,
        sizeof(errbuf)
    );
}

#ifdef DATAROUTER_WAIT_DIAGNOSTICS
int datarouter_send(
    datarouter_port *port,
    const char *target_service,
    const void *data,
    size_t len
) {
    dr_io_row row=dr_io_begin(port, 1U);
    int rc=datarouter_send_observed_impl(port, target_service, data, len);
    dr_io_end(row, port, rc);
    return rc;
}
#ifdef DATAROUTER_UNSAFE
int datarouter_unsafe_send(
    datarouter_port *port,
    const char *target_service,
    const void *data,
    size_t len
) {
    dr_io_row row=dr_io_begin(port, 2U);
    int rc=datarouter_unsafe_send_observed_impl(port, target_service, data, len);
    dr_io_end(row, port, rc);
    return rc;
}
#endif
int datarouter_recv(
    datarouter_port *port,
    void *buffer,
    size_t capacity,
    datarouter_message_info *info
) {
    dr_io_row row=dr_io_begin(port, 3U);
    int rc=datarouter_recv_observed_impl(port, buffer, capacity, info);
    dr_io_end(row, port, rc);
    return rc;
}
#endif
