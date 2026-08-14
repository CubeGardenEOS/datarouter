#include "datarouter_internal.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#if (defined(__x86_64__) || defined(__i386__)) && (defined(__GNUC__) || defined(__clang__))
#include <cpuid.h>
#include <immintrin.h>
#endif

/* ---- data-plane copy strategy --------------------------------------------
 * The hot-path ring copies dispatch to a per-process copy routine so the
 * same binary can be compared across copy strategies: DATAROUTER_COPY names
 * a strategy from the table below (default "scalar" = libc memcpy, which on
 * x86-64 is already AVX2-tuned: glibc __memmove_avx_unaligned_erms). A
 * hand-rolled 256-bit load/store loop measured consistently SLOWER
 * (docs/benchmark-modes-2026-08), so "avx2" exists only as a benchmark knob.
 *
 * AVX-512 upgrade path: add a copy_avx512 kernel (target("avx512f"), 64-byte
 * loadu/storeu loop) plus one table row below; DATAROUTER_COPY=avx512 then
 * works with no other changes. Only pursue on an AVX-512 CPU where glibc's
 * __memmove_avx512 is not already winning. Selection is process-global and
 * lazy, so benchmarks run each strategy from a separate process without
 * rebuilding. Copies under 64 bytes always use memcpy to skip the dispatch.
 * The strategy also applies to the switch's routed copies, which is why a
 * "safe mode" run pins DATAROUTER_COPY=scalar. */

typedef void (*datarouter_copy_fn)(void *dst, const void *src, size_t n);

#if (defined(__x86_64__) || defined(__i386__)) && (defined(__GNUC__) || defined(__clang__))
__attribute__((target("avx2")))
static void copy_avx2(void *dst, const void *src, size_t n) {
    const unsigned char *s = (const unsigned char *)src;
    unsigned char *d = (unsigned char *)dst;
    size_t i = 0;
    for (; i + 32 <= n; i += 32) {
        __m256i chunk = _mm256_loadu_si256((const __m256i *)(const void *)(s + i));
        _mm256_storeu_si256((__m256i *)(void *)(d + i), chunk);
    }
    if (i < n) {
        memcpy(d + i, s + i, n - i);
    }
}
#else
static void copy_avx2(void *dst, const void *src, size_t n) {
    memcpy(dst, src, n);
}
#endif

static void copy_scalar(void *dst, const void *src, size_t n) {
    memcpy(dst, src, n);
}

#if (defined(__x86_64__) || defined(__i386__)) && (defined(__GNUC__) || defined(__clang__))
static int copy_cpu_supports_avx2(void) {
    return __builtin_cpu_supports("avx2");
}
#else
static int copy_cpu_supports_avx2(void) {
    return 0;
}
#endif

typedef struct {
    const char *name; /* DATAROUTER_COPY value that selects this strategy */
    datarouter_copy_fn fn;
    int (*supported)(void); /* NULL = always available (default) */
} datarouter_copy_strategy_def;

static const datarouter_copy_strategy_def kCopyStrategies[] = {
    { "scalar", copy_scalar, NULL },
    { "avx2", copy_avx2, copy_cpu_supports_avx2 },
    /* AVX-512 (future): { "avx512", copy_avx512, copy_cpu_supports_avx512 }, */
};

static datarouter_copy_fn g_copy_large = copy_scalar;
static const datarouter_copy_strategy_def *g_copy_active = &kCopyStrategies[0];
static pthread_once_t g_copy_once = PTHREAD_ONCE_INIT;

static void copy_init(void) {
    const char *choice = getenv("DATAROUTER_COPY");
    if (choice) {
        for (size_t i = 0; i < sizeof(kCopyStrategies) / sizeof(kCopyStrategies[0]); i++) {
            if (strcmp(choice, kCopyStrategies[i].name) == 0) {
                if (kCopyStrategies[i].supported == NULL || kCopyStrategies[i].supported()) {
                    g_copy_active = &kCopyStrategies[i];
                    g_copy_large = kCopyStrategies[i].fn;
                }
                /* Named but unsupported: keep the default (scalar). */
                return;
            }
        }
    }
    g_copy_active = &kCopyStrategies[0];
    g_copy_large = kCopyStrategies[0].fn;
}

static void datarouter_copy(void *dst, const void *src, size_t n) {
    if (n >= 64) {
        pthread_once(&g_copy_once, copy_init);
        g_copy_large(dst, src, n);
    } else {
        memcpy(dst, src, n);
    }
}

const char *datarouter_copy_strategy(void) {
    pthread_once(&g_copy_once, copy_init);
    return g_copy_active->name;
}

static uint32_t ring_masked(uint32_t value) {
    return value & (DATAROUTER_RING_CAPACITY - 1U);
}

void datarouter_ring_reset(datarouter_channel *channel) {
    if (!channel) {
        return;
    }
    atomic_store_explicit(&channel->header.head, 0U, memory_order_release);
    atomic_store_explicit(&channel->header.tail, 0U, memory_order_release);
}

size_t datarouter_ring_used(const datarouter_channel *channel) {
    size_t used = 0U;
    return datarouter_ring_validate(channel, &used) == DATAROUTER_OK ? used : SIZE_MAX;
}

size_t datarouter_ring_free(const datarouter_channel *channel) {
    size_t used = 0U;
    if (datarouter_ring_validate(channel, &used) != DATAROUTER_OK) {
        return 0U;
    }
    return DATAROUTER_RING_CAPACITY - used;
}

int datarouter_ring_validate(const datarouter_channel *channel, size_t *used) {
    if (!channel) {
        return DATAROUTER_ERR_INVALID_ARGUMENT;
    }
    uint32_t head = atomic_load_explicit(&channel->header.head, memory_order_acquire);
    uint32_t tail = atomic_load_explicit(&channel->header.tail, memory_order_acquire);
    size_t current = (size_t)(uint32_t)(head - tail);
    if (current > DATAROUTER_RING_CAPACITY) {
        return DATAROUTER_ERR_PROTOCOL;
    }
    if (used) {
        *used = current;
    }
    return DATAROUTER_OK;
}

static void ring_copy_in(datarouter_channel *channel, uint32_t head, const uint8_t *data, size_t len) {
    if (len == 0U) {
        return;
    }
    uint32_t pos = ring_masked(head);
    size_t first = DATAROUTER_RING_CAPACITY - (size_t)pos;
    if (first > len) {
        first = len;
    }
    datarouter_copy(&channel->data[pos], data, first);
    if (len > first) {
        datarouter_copy(channel->data, data + first, len - first);
    }
}

static void ring_copy_out(const datarouter_channel *channel, uint32_t tail, uint8_t *data, size_t len) {
    if (len == 0U) {
        return;
    }
    uint32_t pos = ring_masked(tail);
    size_t first = DATAROUTER_RING_CAPACITY - (size_t)pos;
    if (first > len) {
        first = len;
    }
    datarouter_copy(data, &channel->data[pos], first);
    if (len > first) {
        datarouter_copy(data + first, channel->data, len - first);
    }
}

int datarouter_ring_write(datarouter_channel *channel, const uint8_t *data, size_t len) {
    if (!channel || (!data && len > 0U)) {
        return DATAROUTER_ERR_INVALID_ARGUMENT;
    }
    if (len > DATAROUTER_RING_CAPACITY) {
        return DATAROUTER_ERR_PAYLOAD_TOO_LARGE;
    }
    size_t used = 0U;
    int state_rc = datarouter_ring_validate(channel, &used);
    if (state_rc != DATAROUTER_OK) {
        return state_rc;
    }
    if (DATAROUTER_RING_CAPACITY - used < len) {
        return DATAROUTER_ERR_FULL;
    }
    uint32_t head = atomic_load_explicit(&channel->header.head, memory_order_relaxed);
    ring_copy_in(channel, head, data, len);
    atomic_store_explicit(&channel->header.head, head + (uint32_t)len, memory_order_release);
    return DATAROUTER_OK;
}

int datarouter_ring_write_parts(
    datarouter_channel *channel,
    const uint8_t *first,
    size_t first_len,
    const uint8_t *second,
    size_t second_len
) {
    if (!channel || (!first && first_len > 0U) || (!second && second_len > 0U)) {
        return DATAROUTER_ERR_INVALID_ARGUMENT;
    }
    if (first_len > DATAROUTER_RING_CAPACITY ||
        second_len > DATAROUTER_RING_CAPACITY - first_len) {
        return DATAROUTER_ERR_PAYLOAD_TOO_LARGE;
    }
    size_t total_len = first_len + second_len;
    size_t used = 0U;
    int state_rc = datarouter_ring_validate(channel, &used);
    if (state_rc != DATAROUTER_OK) {
        return state_rc;
    }
    if (DATAROUTER_RING_CAPACITY - used < total_len) {
        return DATAROUTER_ERR_FULL;
    }
    uint32_t head = atomic_load_explicit(&channel->header.head, memory_order_relaxed);
    ring_copy_in(channel, head, first, first_len);
    ring_copy_in(channel, head + (uint32_t)first_len, second, second_len);
    atomic_store_explicit(&channel->header.head, head + (uint32_t)total_len, memory_order_release);
    return DATAROUTER_OK;
}

int datarouter_ring_peek(const datarouter_channel *channel, uint8_t *data, size_t len) {
    if (!channel || (!data && len > 0U)) {
        return DATAROUTER_ERR_INVALID_ARGUMENT;
    }
    if (len > DATAROUTER_RING_CAPACITY) {
        return DATAROUTER_ERR_PAYLOAD_TOO_LARGE;
    }
    size_t used = 0U;
    int state_rc = datarouter_ring_validate(channel, &used);
    if (state_rc != DATAROUTER_OK) {
        return state_rc;
    }
    if (used < len) {
        return DATAROUTER_ERR_AGAIN;
    }
    uint32_t tail = atomic_load_explicit(&channel->header.tail, memory_order_relaxed);
    ring_copy_out(channel, tail, data, len);
    return DATAROUTER_OK;
}

int datarouter_ring_read(datarouter_channel *channel, uint8_t *data, size_t len) {
    if (!channel || (!data && len > 0U)) {
        return DATAROUTER_ERR_INVALID_ARGUMENT;
    }
    if (len > DATAROUTER_RING_CAPACITY) {
        return DATAROUTER_ERR_PAYLOAD_TOO_LARGE;
    }
    size_t used = 0U;
    int state_rc = datarouter_ring_validate(channel, &used);
    if (state_rc != DATAROUTER_OK) {
        return state_rc;
    }
    if (used < len) {
        return DATAROUTER_ERR_AGAIN;
    }
    uint32_t tail = atomic_load_explicit(&channel->header.tail, memory_order_relaxed);
    ring_copy_out(channel, tail, data, len);
    atomic_store_explicit(&channel->header.tail, tail + (uint32_t)len, memory_order_release);
    return DATAROUTER_OK;
}

int datarouter_ring_skip(datarouter_channel *channel, size_t len) {
    if (!channel) {
        return DATAROUTER_ERR_INVALID_ARGUMENT;
    }
    if (len > DATAROUTER_RING_CAPACITY) {
        return DATAROUTER_ERR_PAYLOAD_TOO_LARGE;
    }
    size_t used = 0U;
    int state_rc = datarouter_ring_validate(channel, &used);
    if (state_rc != DATAROUTER_OK) {
        return state_rc;
    }
    if (used < len) {
        return DATAROUTER_ERR_AGAIN;
    }
    uint32_t tail = atomic_load_explicit(&channel->header.tail, memory_order_relaxed);
    atomic_store_explicit(&channel->header.tail, tail + (uint32_t)len, memory_order_release);
    return DATAROUTER_OK;
}

static void ring_copy_between(
    const datarouter_channel *source,
    uint32_t source_index,
    datarouter_channel *target,
    uint32_t target_index,
    size_t len
) {
    while (len > 0U) {
        uint32_t source_pos = ring_masked(source_index);
        uint32_t target_pos = ring_masked(target_index);
        size_t chunk = DATAROUTER_RING_CAPACITY - (size_t)source_pos;
        size_t target_contiguous = DATAROUTER_RING_CAPACITY - (size_t)target_pos;
        if (chunk > target_contiguous) {
            chunk = target_contiguous;
        }
        if (chunk > len) {
            chunk = len;
        }
        datarouter_copy(&target->data[target_pos], &source->data[source_pos], chunk);
        source_index += (uint32_t)chunk;
        target_index += (uint32_t)chunk;
        len -= chunk;
    }
}

int datarouter_ring_transfer_frame(
    datarouter_channel *source,
    datarouter_channel *target,
    const uint8_t *header,
    size_t header_len,
    size_t source_frame_len
) {
    if (!source || !target || (!header && header_len > 0U) || source == target ||
        header_len != DATAROUTER_FRAME_HEADER_SIZE || header_len > source_frame_len) {
        return DATAROUTER_ERR_INVALID_ARGUMENT;
    }
    if (source_frame_len > DATAROUTER_RING_CAPACITY) {
        return DATAROUTER_ERR_PAYLOAD_TOO_LARGE;
    }
    size_t source_used = 0U;
    size_t target_used = 0U;
    int rc = datarouter_ring_validate(source, &source_used);
    if (rc != DATAROUTER_OK) {
        return rc;
    }
    rc = datarouter_ring_validate(target, &target_used);
    if (rc != DATAROUTER_OK) {
        return rc;
    }
    if (source_used < source_frame_len) {
        return DATAROUTER_ERR_AGAIN;
    }
    if (DATAROUTER_RING_CAPACITY - target_used < source_frame_len) {
        return DATAROUTER_ERR_FULL;
    }

    uint32_t source_tail = atomic_load_explicit(&source->header.tail, memory_order_relaxed);
    uint32_t target_head = atomic_load_explicit(&target->header.head, memory_order_relaxed);
    ring_copy_in(target, target_head, header, header_len);
    size_t payload_len = source_frame_len - header_len;
    ring_copy_between(
        source,
        source_tail + (uint32_t)header_len,
        target,
        target_head + (uint32_t)header_len,
        payload_len
    );
    atomic_store_explicit(
        &target->header.head,
        target_head + (uint32_t)source_frame_len,
        memory_order_release
    );
    atomic_store_explicit(
        &source->header.tail,
        source_tail + (uint32_t)source_frame_len,
        memory_order_release
    );
    return DATAROUTER_OK;
}
