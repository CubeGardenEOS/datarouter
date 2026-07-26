#include "datarouter_internal.h"

#include <stdatomic.h>
#include <string.h>

static uint32_t ring_masked(uint32_t value) {
    return value % (uint32_t)DATAROUTER_RING_CAPACITY;
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
    memcpy(&channel->data[pos], data, first);
    if (len > first) {
        memcpy(channel->data, data + first, len - first);
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
    memcpy(data, &channel->data[pos], first);
    if (len > first) {
        memcpy(data + first, channel->data, len - first);
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
        memcpy(&target->data[target_pos], &source->data[source_pos], chunk);
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
