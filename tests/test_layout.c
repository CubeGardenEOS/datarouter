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

static void test_layout(uint32_t ports) {
    size_t size = datarouter_shared_memory_size(ports);
    datarouter_shared_memory *shm = (datarouter_shared_memory *)calloc(1U, size);
    require_int(shm != NULL, "allocate shared memory");
    require_int(datarouter_init_shared_memory(shm, ports) == DATAROUTER_OK, "init layout");

    require_int(datarouter_validate_shared_memory(shm, size) == DATAROUTER_OK, "valid layout");
    require_int(shm->control.max_ports == ports, "configured ports");
    require_int(shm->control.channel_count == ports * 2U, "configured channels");
    require_int(shm->control.total_size == size, "computed total size");
    require_int(sizeof(datarouter_channel) == DATAROUTER_CHANNEL_SIZE, "channel is exactly 512KiB");
    require_int(
        size == sizeof(datarouter_control_plane) + ((size_t)ports * 2U * DATAROUTER_CHANNEL_SIZE),
        "data plane matches channel count"
    );

    for (uint32_t i = 0; i < ports; i++) {
        require_int(shm->control.slots[i].slot_index == i, "slot index");
        require_int(shm->control.slots[i].rx_channel_index == i * 2U, "rx index");
        require_int(shm->control.slots[i].tx_channel_index == i * 2U + 1U, "tx index");

        uint8_t *rx = (uint8_t *)datarouter_rx_channel(shm, i);
        uint8_t *tx = (uint8_t *)datarouter_tx_channel(shm, i);
        require_int((size_t)(tx - rx) == DATAROUTER_CHANNEL_SIZE, "rx/tx pair contiguous");
    }

    uint8_t *base = (uint8_t *)shm + shm->control.data_offset;
    uint8_t *last = base + ((size_t)shm->control.channel_count - 1U) * DATAROUTER_CHANNEL_SIZE;
    require_int(
        (size_t)(last - base) == ((size_t)ports * 2U - 1U) * DATAROUTER_CHANNEL_SIZE,
        "all channels contiguous"
    );
    require_int(datarouter_rx_channel(shm, ports) == NULL, "slot past configured ports is invalid");
    free(shm);
}

int main(void) {
    require_int(DATAROUTER_DEFAULT_MAX_PORTS == 4, "legacy default remains four ports");
    require_int(DATAROUTER_CHANNEL_SIZE == 524288U, "default channel size is 512KiB");
    require_int(DATAROUTER_FRAME_HEADER_SIZE == 8U, "frame header carries u32 route and length");
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
