#include "datarouter_router.h"
#include "datarouter_registry.h"
#include "datarouter_scheduler.h"
#include "datarouter_signal.h"

#include <stdatomic.h>

#ifdef DATAROUTER_TEST_HOOKS
void (*datarouter_test_after_full_before_block)(
    datarouter_shared_memory *shm,
    uint32_t target_slot
) = NULL;
void (*datarouter_test_after_rx_publish)(
    datarouter_shared_memory *shm,
    uint32_t source_route,
    uint32_t target_slot
) = NULL;
#endif

static void encode_frame_header(
    uint8_t header[DATAROUTER_FRAME_HEADER_SIZE],
    uint32_t route_id,
    uint32_t payload_len
) {
    header[0] = (uint8_t)(route_id & 0xffU);
    header[1] = (uint8_t)((route_id >> 8U) & 0xffU);
    header[2] = (uint8_t)((route_id >> 16U) & 0xffU);
    header[3] = (uint8_t)((route_id >> 24U) & 0xffU);
    header[4] = (uint8_t)(payload_len & 0xffU);
    header[5] = (uint8_t)((payload_len >> 8U) & 0xffU);
    header[6] = (uint8_t)((payload_len >> 16U) & 0xffU);
    header[7] = (uint8_t)((payload_len >> 24U) & 0xffU);
}

static int decode_route_header(
    datarouter_channel *tx,
    uint32_t *route_id,
    uint32_t *payload_len
) {
    if (!route_id || !payload_len) {
        return DATAROUTER_ERR_INVALID_ARGUMENT;
    }
    size_t used = 0U;
    int rc = datarouter_ring_validate(tx, &used);
    if (rc != DATAROUTER_OK) {
        return rc;
    }
    if (used < DATAROUTER_FRAME_HEADER_SIZE) {
        return used == 0U ? DATAROUTER_ERR_AGAIN : DATAROUTER_ERR_PROTOCOL;
    }
    uint8_t header[DATAROUTER_FRAME_HEADER_SIZE];
    rc = datarouter_ring_peek(tx, header, sizeof(header));
    if (rc != DATAROUTER_OK) {
        return rc;
    }
    *route_id = (uint32_t)header[0] |
                ((uint32_t)header[1] << 8U) |
                ((uint32_t)header[2] << 16U) |
                ((uint32_t)header[3] << 24U);
    *payload_len = (uint32_t)header[4] |
                   ((uint32_t)header[5] << 8U) |
                   ((uint32_t)header[6] << 16U) |
                   ((uint32_t)header[7] << 24U);
    if (*route_id == 0U ||
        *payload_len > (uint32_t)(DATAROUTER_RING_CAPACITY - DATAROUTER_FRAME_HEADER_SIZE)) {
        return DATAROUTER_ERR_PROTOCOL;
    }
    size_t frame_len = DATAROUTER_FRAME_HEADER_SIZE + (size_t)*payload_len;
    return used >= frame_len ? DATAROUTER_OK : DATAROUTER_ERR_PROTOCOL;
}

static int tx_has_complete_frame(datarouter_shared_memory *shm, uint32_t source_slot) {
    datarouter_channel *tx = datarouter_tx_channel(shm, source_slot);
    uint32_t route_id = 0U;
    uint32_t payload_len = 0U;
    return decode_route_header(tx, &route_id, &payload_len) == DATAROUTER_OK;
}

static void isolate_corrupt_slot(datarouter_shared_memory *shm, uint32_t slot_index) {
    if (!shm || slot_index >= datarouter_active_max_ports(shm)) {
        return;
    }
    atomic_fetch_add_explicit(&shm->control.corrupted_rings, 1ULL, memory_order_relaxed);
    datarouter_registry_release_slot(shm, slot_index);
    datarouter_signal_post(&shm->control.rx_signal[slot_index]);
}

static int route_one_frame(
    datarouter_shared_memory *shm,
    uint32_t source_slot,
    int *frame_consumed
) {
    if (frame_consumed) {
        *frame_consumed = 0;
    }
    datarouter_registry_entry *source = datarouter_registry_find_routable_by_slot(shm, source_slot);
    if (!source) {
        return DATAROUTER_ERR_NOT_FOUND;
    }
    datarouter_registry_metadata source_metadata;
    if (!datarouter_registry_read_routable(source, &source_metadata)) {
        return DATAROUTER_ERR_NOT_FOUND;
    }
    datarouter_channel *tx = datarouter_tx_channel(shm, source_slot);
    if (!tx) {
        return DATAROUTER_ERR_INTERNAL;
    }

    uint32_t route_id = 0U;
    uint32_t payload_len = 0U;
    int rc = decode_route_header(tx, &route_id, &payload_len);
    if (rc == DATAROUTER_ERR_PROTOCOL) {
        isolate_corrupt_slot(shm, source_slot);
        return rc;
    }
    if (rc != DATAROUTER_OK) {
        return rc;
    }
    size_t frame_len = DATAROUTER_FRAME_HEADER_SIZE + (size_t)payload_len;

    datarouter_registry_entry *target = datarouter_registry_find_active_by_route(shm, route_id);
    if (!target) {
        rc = datarouter_ring_skip(tx, frame_len);
        if (rc == DATAROUTER_OK && frame_consumed) {
            *frame_consumed = 1;
        }
        if (rc == DATAROUTER_ERR_PROTOCOL) {
            isolate_corrupt_slot(shm, source_slot);
            return rc;
        }
        return rc == DATAROUTER_OK ? DATAROUTER_ERR_NOT_FOUND : rc;
    }
    uint32_t target_slot = target->slot_index;
    datarouter_channel *rx = datarouter_rx_channel(shm, target_slot);
    if (!rx) {
        return DATAROUTER_ERR_INTERNAL;
    }
    size_t rx_used = 0U;
    rc = datarouter_ring_validate(rx, &rx_used);
    if (rc == DATAROUTER_ERR_PROTOCOL) {
        isolate_corrupt_slot(shm, target_slot);
        if (datarouter_ring_skip(tx, frame_len) == DATAROUTER_OK && frame_consumed) {
            *frame_consumed = 1;
        }
        return rc;
    }
    if (rc != DATAROUTER_OK) {
        return rc;
    }
    if (DATAROUTER_RING_CAPACITY - rx_used < frame_len) {
#ifdef DATAROUTER_TEST_HOOKS
        if (datarouter_test_after_full_before_block) {
            datarouter_test_after_full_before_block(shm, target_slot);
        }
#endif
        /* Prepare-to-wait before rechecking space. If the receiver freed RX
         * before this blocked bit became visible, the acquire/release RMW and
         * second tail read observe that space. Otherwise the receiver sees the
         * blocked bit and requeues this source. */
        datarouter_scheduler_mark_tx_blocked(shm, source_slot);
        rc = datarouter_ring_validate(rx, &rx_used);
        if (rc == DATAROUTER_ERR_PROTOCOL) {
            isolate_corrupt_slot(shm, target_slot);
            return rc;
        }
        if (rc != DATAROUTER_OK || DATAROUTER_RING_CAPACITY - rx_used < frame_len) {
            return rc == DATAROUTER_OK ? DATAROUTER_ERR_FULL : rc;
        }
    }

    uint8_t header[DATAROUTER_FRAME_HEADER_SIZE];
    encode_frame_header(header, source_metadata.route_id, payload_len);
    /* Account for the frame before publishing the RX head. The receiver may
     * consume immediately after that release store, so publishing first would
     * allow its decrement to race ahead of this increment and strand the
     * source slot forever. */
    datarouter_registry_track_inflight(shm, source_slot, target_slot);
    rc = datarouter_ring_transfer_frame(tx, rx, header, sizeof(header), frame_len);
    if (rc != DATAROUTER_OK) {
        datarouter_registry_complete_inflight(
            shm,
            source_metadata.route_id,
            target_slot
        );
    }
    if (rc == DATAROUTER_ERR_PROTOCOL) {
        isolate_corrupt_slot(shm, source_slot);
        return rc;
    }
    if (rc != DATAROUTER_OK) {
        return rc;
    }
#ifdef DATAROUTER_TEST_HOOKS
    if (datarouter_test_after_rx_publish) {
        datarouter_test_after_rx_publish(shm, source_metadata.route_id, target_slot);
    }
#endif
    if (frame_consumed) {
        *frame_consumed = 1;
    }
    datarouter_signal_post(&shm->control.rx_signal[target_slot]);
    datarouter_touch_entry(source, datarouter_now_ns());
    datarouter_scheduler_clear_tx_blocked(shm, source_slot);
    return DATAROUTER_OK;
}

uint32_t datarouter_route_frames_budget(
    datarouter_shared_memory *shm,
    datarouter_route_stats *stats,
    uint32_t max_frames,
    uint32_t budget_us
) {
    if (!shm || max_frames == 0U) {
        return 0U;
    }
    uint64_t started = datarouter_now_ns();
    uint64_t budget_ns = (uint64_t)budget_us * 1000ULL;
    uint32_t routed = 0U;
    uint32_t attempted = 0U;
    while (attempted < max_frames) {
        if (budget_ns > 0ULL && attempted > 0U && datarouter_now_ns() - started >= budget_ns) {
            break;
        }
        uint32_t slot = datarouter_scheduler_take_next_ready(shm);
        if (slot == UINT32_MAX) {
            break;
        }
        attempted++;
        unsigned int slot_state = slot < datarouter_active_max_ports(shm)
                                      ? atomic_load_explicit(
                                            &shm->control.registry[slot].state,
                                            memory_order_acquire
                                        )
                                      : DATAROUTER_PORT_EMPTY;
        if (slot >= datarouter_active_max_ports(shm) ||
            (slot_state != DATAROUTER_PORT_ACTIVE &&
             slot_state != DATAROUTER_PORT_DRAINING)) {
            continue;
        }

        int consumed = 0;
        int rc = route_one_frame(shm, slot, &consumed);
        if (rc == DATAROUTER_OK) {
            routed++;
            if (stats) {
                stats->routed++;
            }
        } else if (rc == DATAROUTER_ERR_FULL) {
            if (stats) {
                stats->full++;
            }
            /* The prepare-to-wait protocol in route_one_frame already marked it. */
        } else if (rc == DATAROUTER_ERR_NOT_FOUND) {
            if (stats) {
                stats->not_found++;
            }
        } else if (rc == DATAROUTER_ERR_PROTOCOL) {
            if (stats) {
                stats->protocol++;
                stats->corrupted++;
            }
        } else if (rc == DATAROUTER_ERR_INTERNAL) {
            if (stats) {
                stats->internal++;
            }
        }

        if (consumed && tx_has_complete_frame(shm, slot)) {
            datarouter_scheduler_mark_tx_ready(shm, slot);
        }
    }
    return routed;
}

uint32_t datarouter_route_frames_stats(datarouter_shared_memory *shm, datarouter_route_stats *stats) {
    return datarouter_route_frames_budget(
        shm,
        stats,
        DATAROUTER_SWITCH_ROUTE_MAX_FRAMES,
        DATAROUTER_SWITCH_ROUTE_BUDGET_US
    );
}

uint32_t datarouter_route_frames(datarouter_shared_memory *shm) {
    return datarouter_route_frames_stats(shm, NULL);
}
