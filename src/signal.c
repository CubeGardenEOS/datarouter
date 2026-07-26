#include "datarouter_signal.h"

#include <stdatomic.h>
#include <time.h>

#ifdef _WIN32
#else
#include <errno.h>
#endif

int datarouter_signal_init(datarouter_signal *signal) {
    if (!signal) {
        return DATAROUTER_ERR_INVALID_ARGUMENT;
    }
#ifdef _WIN32
    atomic_store_explicit(signal, 0U, memory_order_release);
    return DATAROUTER_OK;
#else
    return sem_init(signal, 1, 0) == 0 ? DATAROUTER_OK : DATAROUTER_ERR_PLATFORM;
#endif
}

void datarouter_signal_post(datarouter_signal *signal) {
    if (!signal) {
        return;
    }
#ifdef _WIN32
    atomic_store_explicit(signal, 1U, memory_order_release);
#else
    (void)sem_post(signal);
#endif
}

int datarouter_signal_wait(datarouter_signal *signal, uint32_t timeout_ms) {
    if (!signal) {
        return DATAROUTER_ERR_INVALID_ARGUMENT;
    }
#ifdef _WIN32
    uint64_t started = datarouter_now_ns();
    uint64_t timeout_ns = (uint64_t)timeout_ms * 1000000ULL;
    for (;;) {
        unsigned int expected = 1U;
        if (atomic_compare_exchange_strong_explicit(
                signal,
                &expected,
                0U,
                memory_order_acq_rel,
                memory_order_acquire
            )) {
            return DATAROUTER_OK;
        }
        if (timeout_ms > 0U && datarouter_now_ns() - started >= timeout_ns) {
            return DATAROUTER_ERR_TIMEOUT;
        }
        datarouter_sleep_us(1000U);
    }
#else
    if (timeout_ms == 0U) {
        while (sem_wait(signal) != 0) {
            if (errno != EINTR) {
                return DATAROUTER_ERR_PLATFORM;
            }
        }
        return DATAROUTER_OK;
    }

    struct timespec deadline;
    if (clock_gettime(CLOCK_REALTIME, &deadline) != 0) {
        return DATAROUTER_ERR_PLATFORM;
    }
    deadline.tv_sec += (time_t)(timeout_ms / 1000U);
    deadline.tv_nsec += (long)(timeout_ms % 1000U) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec += deadline.tv_nsec / 1000000000L;
        deadline.tv_nsec %= 1000000000L;
    }
    while (sem_timedwait(signal, &deadline) != 0) {
        if (errno == EINTR) {
            continue;
        }
        if (errno == ETIMEDOUT) {
            return DATAROUTER_ERR_TIMEOUT;
        }
        return DATAROUTER_ERR_PLATFORM;
    }
    return DATAROUTER_OK;
#endif
}
