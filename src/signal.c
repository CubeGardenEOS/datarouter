#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "datarouter_signal.h"

#include <limits.h>
#include <time.h>
#ifndef _WIN32
#include <errno.h>
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

#include "signal_diagnostics.h"

/* A notification is a hint to recheck a predicate, not a frame count.
 * Callers snapshot the generation BEFORE reading their queue/mailbox. A
 * concurrent post then either changes the futex value or wakes every waiter.
 * Thus another receiver consuming a frame cannot consume our notification.
 * Posts without sleepers make no system call and retain no semaphore debt. */
int datarouter_signal_init(datarouter_signal *signal) {
    if (!signal) return DATAROUTER_ERR_INVALID_ARGUMENT;
    atomic_init(&signal->sequence, 0U);
    atomic_init(&signal->consumed, 0U);
    atomic_init(&signal->waiters, 0U);
    return DATAROUTER_OK;
}

uint32_t datarouter_signal_prepare(const datarouter_signal *signal) {
    return signal ? atomic_load_explicit(&signal->sequence, memory_order_seq_cst) : 0U;
}

void datarouter_signal_post(datarouter_signal *signal) {
    if (!signal) return;
    atomic_fetch_add_explicit(&signal->sequence, 1U, memory_order_seq_cst);
#ifndef _WIN32
    /* Together with the waiter's increment then sequence check, seq_cst
     * forbids both sides from missing each other across these two atomics. */
    if (atomic_load_explicit(&signal->waiters, memory_order_seq_cst) != 0U) {
        DR_SIGNAL_BEGIN(signal, datarouter_signal_prepare(signal), 2);
        long rc = syscall(SYS_futex, &signal->sequence, FUTEX_WAKE, INT_MAX, NULL, NULL, 0);
        DR_SIGNAL_END(rc, rc < 0 ? errno : 0, signal);
        (void)rc;
    }
#endif
}

void datarouter_signal_post_if_waiting(datarouter_signal *signal) {
    if (!signal) return;
    /* Publish predicate -> full fence -> observe sleepers. Paired with arm's
     * RMW/fence -> predicate recheck, the C11 SC-fence order forbids both
     * sides observing the old value (store-buffering litmus). */
    atomic_thread_fence(memory_order_seq_cst);
    if (atomic_load_explicit(&signal->waiters, memory_order_seq_cst))
        datarouter_signal_post(signal);
}

int datarouter_signal_arm(datarouter_signal *signal) {
    if (!signal) return DATAROUTER_ERR_INVALID_ARGUMENT;
    unsigned int waiters = atomic_load_explicit(&signal->waiters, memory_order_relaxed);
    do {
        /* A killed waiter can leave a conservative count. Never let the
         * counter wrap to zero and suppress wakes for remaining processes. */
        if (waiters == UINT_MAX) return DATAROUTER_ERR_PLATFORM;
    } while (!atomic_compare_exchange_weak_explicit(&signal->waiters, &waiters,
                 waiters + 1U, memory_order_seq_cst, memory_order_relaxed));
    atomic_thread_fence(memory_order_seq_cst);
    return DATAROUTER_OK;
}

void datarouter_signal_cancel(datarouter_signal *signal) {
    atomic_fetch_sub_explicit(&signal->waiters, 1U, memory_order_seq_cst);
}

int datarouter_signal_wait_armed(datarouter_signal *signal, uint32_t sequence,
                               uint32_t timeout_ms) {
    if (!signal) return DATAROUTER_ERR_INVALID_ARGUMENT;
    uint64_t started = datarouter_now_ns();
    uint64_t budget = (uint64_t)timeout_ms * 1000000ULL;
    int result = DATAROUTER_OK;
    for (;;) {
        if (datarouter_signal_prepare(signal) != sequence) {
            DR_SIGNAL_BEGIN(signal, sequence, 3);
            DR_SIGNAL_END(0, 0, signal);
            break;
        }
        uint64_t elapsed = datarouter_now_ns() - started;
        if (timeout_ms && elapsed >= budget) {
            result = DATAROUTER_ERR_TIMEOUT;
            break;
        }
#ifdef _WIN32
        /* WaitOnAddress is process-local. Keep the existing portable polling
         * backend, but compare generations so all concurrent waiters wake. */
        datarouter_sleep_us(1000U);
#else
        struct timespec remaining;
        struct timespec *duration = NULL;
        if (timeout_ms) {
            uint64_t ns = budget - elapsed;
            remaining.tv_sec = (time_t)(ns / 1000000000ULL);
            remaining.tv_nsec = (long)(ns % 1000000000ULL);
            duration = &remaining;
        }
        DR_SIGNAL_BEGIN(signal, sequence, 1);
        long rc = syscall(SYS_futex, &signal->sequence, FUTEX_WAIT, sequence,
                          duration, NULL, 0);
        int wait_errno = rc < 0 ? errno : 0;
        DR_SIGNAL_END(rc, wait_errno, signal);
        if (rc == 0) break; /* a wake is only a hint; caller checks predicate */
        if (wait_errno == EAGAIN || wait_errno == EINTR) continue;
        if (wait_errno == ETIMEDOUT) {
            result = datarouter_signal_prepare(signal) != sequence
                ? DATAROUTER_OK : DATAROUTER_ERR_TIMEOUT;
        } else result = DATAROUTER_ERR_PLATFORM;
        break;
#endif
    }
    datarouter_signal_cancel(signal);
    return result;
}

int datarouter_signal_wait_prepared(datarouter_signal *signal, uint32_t sequence,
                                   uint32_t timeout_ms) {
    int rc = datarouter_signal_arm(signal);
    if (rc != DATAROUTER_OK) return rc;
    return datarouter_signal_wait_armed(signal, sequence, timeout_ms);
}

int datarouter_signal_wait(datarouter_signal *signal, uint32_t timeout_ms) {
    if (!signal) return DATAROUTER_ERR_INVALID_ARGUMENT;
    uint32_t consumed = atomic_load_explicit(&signal->consumed, memory_order_acquire);
    int result = datarouter_signal_wait_prepared(signal, consumed, timeout_ms);
    if (result == DATAROUTER_OK)
        atomic_store_explicit(&signal->consumed, datarouter_signal_prepare(signal), memory_order_release);
    return result;
}

#if defined(DATAROUTER_WAIT_DIAGNOSTICS) && !defined(_WIN32)
void datarouter_diagnostic_switch(datarouter_signal *signal, uint32_t kind) {
    DR_SIGNAL_BEGIN(signal, datarouter_signal_prepare(signal), kind);
    DR_SIGNAL_END(0, 0, signal);
}
#endif

#if defined(DATAROUTER_WAIT_DIAGNOSTICS) && !defined(_WIN32)
void datarouter_diagnostic_flush(void) { dr_signal_dump(); }
#endif
