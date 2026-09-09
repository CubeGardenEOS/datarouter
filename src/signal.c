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
    if (atomic_load_explicit(&signal->waiters, memory_order_seq_cst) != 0U)
        (void)syscall(SYS_futex, &signal->sequence, FUTEX_WAKE, INT_MAX, NULL, NULL, 0);
#endif
}

int datarouter_signal_wait_prepared(datarouter_signal *signal, uint32_t sequence,
                                   uint32_t timeout_ms) {
    if (!signal) return DATAROUTER_ERR_INVALID_ARGUMENT;
    unsigned int waiters = atomic_load_explicit(&signal->waiters, memory_order_relaxed);
    do {
        /* A killed waiter can leave a conservative count. Never let the
         * counter wrap to zero and suppress wakes for remaining processes. */
        if (waiters == UINT_MAX) return DATAROUTER_ERR_PLATFORM;
    } while (!atomic_compare_exchange_weak_explicit(&signal->waiters, &waiters,
                 waiters + 1U, memory_order_seq_cst, memory_order_relaxed));
    uint64_t started = datarouter_now_ns();
    uint64_t budget = (uint64_t)timeout_ms * 1000000ULL;
    int result = DATAROUTER_OK;
    for (;;) {
        if (datarouter_signal_prepare(signal) != sequence) break;
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
        long rc = syscall(SYS_futex, &signal->sequence, FUTEX_WAIT, sequence,
                          duration, NULL, 0);
        if (rc == 0) break; /* a wake is only a hint; caller checks predicate */
        if (errno == EAGAIN || errno == EINTR) continue;
        if (errno == ETIMEDOUT) {
            result = datarouter_signal_prepare(signal) != sequence
                ? DATAROUTER_OK : DATAROUTER_ERR_TIMEOUT;
        } else result = DATAROUTER_ERR_PLATFORM;
        break;
#endif
    }
    atomic_fetch_sub_explicit(&signal->waiters, 1U, memory_order_seq_cst);
    return result;
}

int datarouter_signal_wait(datarouter_signal *signal, uint32_t timeout_ms) {
    if (!signal) return DATAROUTER_ERR_INVALID_ARGUMENT;
    uint32_t consumed = atomic_load_explicit(&signal->consumed, memory_order_acquire);
    int result = datarouter_signal_wait_prepared(signal, consumed, timeout_ms);
    if (result == DATAROUTER_OK)
        atomic_store_explicit(&signal->consumed, datarouter_signal_prepare(signal), memory_order_release);
    return result;
}
