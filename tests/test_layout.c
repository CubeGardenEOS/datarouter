#include "../src/datarouter_internal.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

static void require_int(int ok, const char *message) {
    if (!ok) {
        fprintf(stderr, "test_layout failed: %s\n", message);
        exit(1);
    }
}

#if DATAROUTER_LAYOUT_VERSION >= 18U
static void test_layout(uint32_t ports) {
    size_t size = datarouter_shared_memory_size(ports);
    datarouter_shared_memory *shm = calloc(1, size);
    require_int(shm != NULL, "allocate shared memory");
    /* Initialization must not first-touch payload pages. Sentinel at first
     * and last payload byte survives initialization. */
    uint8_t *bytes = (uint8_t *)shm;
    bytes[DATAROUTER_DATA_OFFSET(ports)] = 0xa5;
    bytes[size - 1] = 0x5a;
    require_int(datarouter_init_shared_memory(shm, ports) == DATAROUTER_OK, "init layout");
    require_int(bytes[DATAROUTER_DATA_OFFSET(ports)] == 0xa5 && bytes[size-1] == 0x5a, "payload not cleared");
    require_int(datarouter_validate_shared_memory(shm, size) == DATAROUTER_OK, "valid mapping");
    require_int(datarouter_validate_shared_memory(shm, size-1) == DATAROUTER_ERR_PROTOCOL, "short mapping");
    require_int(shm->control.channel_count == ports + ports*ports, "N + N squared queues");
    require_int(shm->control.data_offset % 4096 == 0, "page aligned payload base");
    for (uint32_t source = 0; source < ports; source++) {
        datarouter_channel tx = datarouter_tx_channel(shm, source);
        require_int(tx.data == bytes + shm->control.data_offset + (size_t)source * DATAROUTER_RING_CAPACITY, "TX index");
        for (uint32_t target = 0; target < ports; target++) {
            datarouter_channel rx = datarouter_rx_channel(shm, source, target);
            size_t index = ports + (size_t)source * ports + target;
            require_int(rx.data == bytes + shm->control.data_offset + index * DATAROUTER_RING_CAPACITY, "pair index");
            require_int((size_t)(rx.data-bytes) % 4096 == 0, "payload page aligned");
            require_int((size_t)((uint8_t *)rx.header-bytes) % 64 == 0, "header cache aligned");
            require_int(datarouter_ring_used(rx) == 0, "empty queue");
        }
    }
    require_int(!datarouter_rx_channel(shm, ports, 0).header, "source overflow");
    require_int(!datarouter_rx_channel(shm, 0, ports).header, "target overflow");
    require_int(!datarouter_rx_channel(shm, UINT32_MAX, UINT32_MAX).header, "index overflow");
    require_int(!datarouter_tx_channel(shm, ports).header, "TX overflow");
    shm->control.layout_version = 17;
    require_int(datarouter_validate_shared_memory(shm, size) == DATAROUTER_ERR_PROTOCOL, "old layout rejected");
    free(shm);
}
#else
static void test_layout(uint32_t ports) {
    size_t size = datarouter_shared_memory_size(ports);
    datarouter_shared_memory *shm = (datarouter_shared_memory *)calloc(1U, size);
    require_int(shm != NULL, "allocate shared memory");
    require_int(datarouter_init_shared_memory(shm, ports) == DATAROUTER_OK, "init layout");

    require_int(datarouter_validate_shared_memory(shm, size) == DATAROUTER_OK, "valid layout");
    require_int(shm->control.max_ports == ports, "configured ports");
    require_int(shm->control.channel_count == ports * 2U, "configured channels");
    require_int(shm->control.total_size == size, "computed total size");
    require_int(
        sizeof(datarouter_channel) == DATAROUTER_CHANNEL_SIZE + DATAROUTER_RING_HEADER_SIZE,
        "channel is padded header plus power-of-two data capacity"
    );
    require_int(
        size == shm->control.data_offset + ((size_t)ports * 2U * sizeof(datarouter_channel)),
        "data plane matches channel count"
    );
#if DATAROUTER_LAYOUT_VERSION >= 17U
    require_int(shm->control.data_offset % 64U == 0U, "data plane aligned to cache line");
    require_int(sizeof(datarouter_channel) % 64U == 0U, "channel stride aligned to cache line");
    require_int(offsetof(datarouter_ring_header, tail) == 64U, "head and tail are separate cache lines");
    shm->control.data_offset += 4U;
    require_int(datarouter_validate_shared_memory(shm, size) == DATAROUTER_ERR_PROTOCOL, "misaligned data plane rejected");
    shm->control.data_offset -= 4U;
#endif

    for (uint32_t i = 0; i < ports; i++) {
        require_int(shm->control.slots[i].slot_index == i, "slot index");
        require_int(shm->control.slots[i].rx_channel_index == i * 2U, "rx index");
        require_int(shm->control.slots[i].tx_channel_index == i * 2U + 1U, "tx index");

        uint8_t *rx = (uint8_t *)datarouter_rx_channel(shm, i);
        uint8_t *tx = (uint8_t *)datarouter_tx_channel(shm, i);
        require_int((size_t)(tx - rx) == sizeof(datarouter_channel), "rx/tx pair contiguous");
    }

    uint8_t *base = (uint8_t *)shm + shm->control.data_offset;
    uint8_t *last = base + ((size_t)shm->control.channel_count - 1U) * sizeof(datarouter_channel);
    require_int(
        (size_t)(last - base) == ((size_t)ports * 2U - 1U) * sizeof(datarouter_channel),
        "all channels contiguous"
    );
    require_int(datarouter_rx_channel(shm, ports) == NULL, "slot past configured ports is invalid");
    free(shm);
}

#endif

int main(void) {
    require_int(DATAROUTER_DEFAULT_MAX_PORTS == 4, "legacy default remains four ports");
    require_int(DATAROUTER_CHANNEL_SIZE == 524288U, "default channel size is 512KiB");
    require_int(DATAROUTER_FRAME_HEADER_SIZE == 8U, "frame header carries u32 route and length");
#if DATAROUTER_LAYOUT_VERSION >= 18U
    require_int(datarouter_shared_memory_size(4) > 10U*1024U*1024U &&
        datarouter_shared_memory_size(4) < 11U*1024U*1024U, "four ports approximately 10MiB");
    require_int(datarouter_shared_memory_size(64) > (size_t)2U*1024U*1024U*1024U &&
        datarouter_shared_memory_size(64) < (size_t)2100U*1024U*1024U, "64 ports approximately 2.03GiB");
    require_int(datarouter_shared_memory_size(0) == 0 && datarouter_shared_memory_size(65) == 0, "capacity bounds");
#else
    require_int(
        datarouter_shared_memory_size(4U) >= 4U * 1024U * 1024U &&
            datarouter_shared_memory_size(4U) < 5U * 1024U * 1024U,
        "four-slot default is approximately 4MiB"
    );
    require_int(
        datarouter_shared_memory_size(6U) >= 6U * 1024U * 1024U &&
            datarouter_shared_memory_size(6U) < 7U * 1024U * 1024U,
        "six-slot config is approximately 6MiB"
    );
    require_int(
        datarouter_shared_memory_size(64U) >= 64U * 1024U * 1024U &&
            datarouter_shared_memory_size(64U) < 65U * 1024U * 1024U,
        "maximum layout is approximately 64MiB"
    );
#endif
    uint32_t high_generation_route = datarouter_route_for_slot_generation(
        63U,
        DATAROUTER_ROUTE_GENERATION_MASK
    );
    require_int(high_generation_route > UINT16_MAX, "route uses the full u32 wire field");
    require_int(
        (high_generation_route >> DATAROUTER_ROUTE_SLOT_BITS) == DATAROUTER_ROUTE_GENERATION_MASK,
        "route preserves all 26 generation bits"
    );
    test_layout(2U);
    test_layout(5U);
    test_layout(6U);

    printf("test_layout ok\n");
    return 0;
}
