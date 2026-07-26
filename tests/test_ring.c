#include "../src/datarouter_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <pthread.h>
#include <stdatomic.h>
#endif

static void require_int(int ok, const char *message) {
    if (!ok) {
        fprintf(stderr, "test_ring failed: %s\n", message);
        exit(1);
    }
}

static void test_counter_wraparound(void) {
    datarouter_channel *channel = (datarouter_channel *)calloc(1U, sizeof(*channel));
    require_int(channel != NULL, "allocate wrap ring");
    uint32_t start = UINT32_MAX - 3U;
    atomic_store_explicit(&channel->header.head, start, memory_order_relaxed);
    atomic_store_explicit(&channel->header.tail, start, memory_order_relaxed);
    const uint8_t input[] = { 1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U };
    uint8_t output[sizeof(input)] = {0};
    require_int(datarouter_ring_write(channel, input, sizeof(input)) == DATAROUTER_OK, "write over u32 wrap");
    require_int(datarouter_ring_used(channel) == sizeof(input), "used survives u32 wrap");
    require_int(datarouter_ring_read(channel, output, sizeof(output)) == DATAROUTER_OK, "read over u32 wrap");
    require_int(memcmp(input, output, sizeof(input)) == 0, "u32 wrap preserves bytes");
    free(channel);
}

static void test_counter_corruption(void) {
    datarouter_channel *channel = (datarouter_channel *)calloc(1U, sizeof(*channel));
    require_int(channel != NULL, "allocate corrupt ring");
    atomic_store_explicit(
        &channel->header.head,
        (uint32_t)DATAROUTER_RING_CAPACITY + 1U,
        memory_order_relaxed
    );
    atomic_store_explicit(&channel->header.tail, 0U, memory_order_relaxed);
    size_t used = 0U;
    uint8_t byte = 0U;
    require_int(datarouter_ring_validate(channel, &used) == DATAROUTER_ERR_PROTOCOL, "corrupt counters rejected");
    require_int(datarouter_ring_used(channel) == SIZE_MAX, "corrupt used is explicit");
    require_int(datarouter_ring_write(channel, &byte, 1U) == DATAROUTER_ERR_PROTOCOL, "corrupt write rejected");
    require_int(datarouter_ring_read(channel, &byte, 1U) == DATAROUTER_ERR_PROTOCOL, "corrupt read rejected");
    free(channel);
}

static void test_direct_transfer(void) {
    datarouter_channel *source = (datarouter_channel *)calloc(1U, sizeof(*source));
    datarouter_channel *target = (datarouter_channel *)calloc(1U, sizeof(*target));
    require_int(source != NULL && target != NULL, "allocate transfer rings");
    uint32_t source_start = (uint32_t)DATAROUTER_RING_CAPACITY - 4U;
    uint32_t target_start = (uint32_t)DATAROUTER_RING_CAPACITY - 2U;
    atomic_store_explicit(&source->header.head, source_start, memory_order_relaxed);
    atomic_store_explicit(&source->header.tail, source_start, memory_order_relaxed);
    atomic_store_explicit(&target->header.head, target_start, memory_order_relaxed);
    atomic_store_explicit(&target->header.tail, target_start, memory_order_relaxed);

    const uint8_t old_header[DATAROUTER_FRAME_HEADER_SIZE] = { 9U, 9U, 9U, 9U, 4U, 0U, 0U, 0U };
    const uint8_t new_header[DATAROUTER_FRAME_HEADER_SIZE] = { 1U, 2U, 3U, 4U, 4U, 0U, 0U, 0U };
    const uint8_t payload[] = { 7U, 8U, 9U, 10U };
    require_int(
        datarouter_ring_write_parts(source, old_header, sizeof(old_header), payload, sizeof(payload)) == DATAROUTER_OK,
        "prepare wrapped source frame"
    );
    require_int(
        datarouter_ring_transfer_frame(
            source,
            target,
            new_header,
            sizeof(new_header),
            sizeof(new_header) + sizeof(payload)
        ) == DATAROUTER_OK,
        "direct wrapped transfer"
    );
    require_int(datarouter_ring_used(source) == 0U, "transfer consumes source atomically");
    require_int(
        datarouter_ring_used(target) == sizeof(new_header) + sizeof(payload),
        "transfer publishes complete target frame"
    );
    uint8_t output[sizeof(new_header) + sizeof(payload)] = {0};
    require_int(datarouter_ring_read(target, output, sizeof(output)) == DATAROUTER_OK, "read transferred frame");
    require_int(memcmp(output, new_header, sizeof(new_header)) == 0, "transfer rewrites header");
    require_int(memcmp(output + sizeof(new_header), payload, sizeof(payload)) == 0, "transfer preserves payload");
    free(target);
    free(source);
}

#ifndef _WIN32
typedef struct {
    datarouter_channel *channel;
    const uint8_t *header;
    const uint8_t *payload;
    size_t payload_len;
    atomic_int started;
    int rc;
} publish_args;

static void *publish_frame(void *opaque) {
    publish_args *args = (publish_args *)opaque;
    atomic_store_explicit(&args->started, 1, memory_order_release);
    args->rc = datarouter_ring_write_parts(
        args->channel,
        args->header,
        DATAROUTER_FRAME_HEADER_SIZE,
        args->payload,
        args->payload_len
    );
    return NULL;
}

static void test_complete_frame_visibility(void) {
    datarouter_channel *channel = (datarouter_channel *)calloc(1U, sizeof(*channel));
    uint8_t *payload = (uint8_t *)malloc(DATAROUTER_RING_CAPACITY / 2U);
    require_int(channel != NULL && payload != NULL, "allocate visibility fixture");
    memset(payload, 0x5a, DATAROUTER_RING_CAPACITY / 2U);
    uint32_t payload_len = (uint32_t)(DATAROUTER_RING_CAPACITY / 2U);
    uint8_t header[DATAROUTER_FRAME_HEADER_SIZE] = {
        1U, 0U, 0U, 0U,
        (uint8_t)(payload_len & 0xffU),
        (uint8_t)((payload_len >> 8U) & 0xffU),
        (uint8_t)((payload_len >> 16U) & 0xffU),
        (uint8_t)((payload_len >> 24U) & 0xffU),
    };
    publish_args args = {
        .channel = channel,
        .header = header,
        .payload = payload,
        .payload_len = payload_len,
        .started = ATOMIC_VAR_INIT(0),
        .rc = DATAROUTER_ERR_INTERNAL,
    };
    pthread_t writer;
    require_int(pthread_create(&writer, NULL, publish_frame, &args) == 0, "start visibility writer");
    while (atomic_load_explicit(&args.started, memory_order_acquire) == 0) {
    }
    size_t complete_len = DATAROUTER_FRAME_HEADER_SIZE + (size_t)payload_len;
    for (;;) {
        size_t used = datarouter_ring_used(channel);
        require_int(used == 0U || used == complete_len, "reader never observes a half frame");
        if (used == complete_len) {
            break;
        }
    }
    require_int(pthread_join(writer, NULL) == 0, "join visibility writer");
    require_int(args.rc == DATAROUTER_OK, "visibility write succeeds");
    free(payload);
    free(channel);
}
#endif

int main(void) {
    datarouter_channel channel;
    datarouter_ring_reset(&channel);
    require_int(datarouter_ring_used(&channel) == 0U, "new ring is empty");

    const uint8_t hello[] = "hello";
    uint8_t out[16];
    require_int(datarouter_ring_write(&channel, hello, sizeof(hello)) == DATAROUTER_OK, "write hello");
    require_int(datarouter_ring_used(&channel) == sizeof(hello), "used bytes");
    require_int(datarouter_ring_peek(&channel, out, sizeof(hello)) == DATAROUTER_OK, "peek hello");
    require_int(memcmp(out, hello, sizeof(hello)) == 0, "peek matches");
    memset(out, 0, sizeof(out));
    require_int(datarouter_ring_read(&channel, out, sizeof(hello)) == DATAROUTER_OK, "read hello");
    require_int(memcmp(out, hello, sizeof(hello)) == 0, "read matches");
    require_int(datarouter_ring_used(&channel) == 0U, "ring empty after read");

    const uint8_t header[] = { 1U, 2U, 3U };
    const uint8_t payload[] = { 4U, 5U, 6U, 7U };
    const uint8_t complete[] = { 1U, 2U, 3U, 4U, 5U, 6U, 7U };
    require_int(
        datarouter_ring_write_parts(&channel, header, sizeof(header), payload, sizeof(payload)) == DATAROUTER_OK,
        "write frame parts"
    );
    require_int(datarouter_ring_used(&channel) == sizeof(complete), "parts publish one complete frame");
    require_int(datarouter_ring_read(&channel, out, sizeof(complete)) == DATAROUTER_OK, "read frame parts");
    require_int(memcmp(out, complete, sizeof(complete)) == 0, "frame parts stay contiguous");

    uint8_t fill[1024];
    memset(fill, 0xab, sizeof(fill));
    size_t written = 0U;
    while (datarouter_ring_free(&channel) >= sizeof(fill)) {
        require_int(datarouter_ring_write(&channel, fill, sizeof(fill)) == DATAROUTER_OK, "fill chunk");
        written += sizeof(fill);
    }
    require_int(written > 0U, "filled bytes");
    require_int(datarouter_ring_write(&channel, fill, sizeof(fill)) == DATAROUTER_ERR_FULL, "full ring");

    test_counter_wraparound();
    test_counter_corruption();
    test_direct_transfer();
#ifndef _WIN32
    test_complete_frame_visibility();
#endif

    printf("test_ring ok\n");
    return 0;
}
