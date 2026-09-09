#include "datarouter_protocol.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>

/* Deterministic sanitizer corpus. This supplements, not replaces, an ongoing
 * coverage-guided fuzzer. Every length describes actually allocated bytes. */
int main(void) {
    uint32_t random = 0x5a17c64;
    uint8_t data[256], output[256];
    for (unsigned iteration = 0; iteration < 100000; ++iteration) {
        for (unsigned i = 0; i < sizeof(data); ++i) {
            random ^= random << 13; random ^= random >> 17; random ^= random << 5;
            data[i] = (uint8_t)random;
        }
        size_t length = iteration % (sizeof(data) + 1);
        datarouter_frame_layout layout;
        datarouter_chunk_meta chunk;
        int valid = datarouter_protocol_validate_tlvs(data, length);
        assert(valid == 0 || valid == 1);
        (void)datarouter_protocol_frame_layout(data, length, &layout);
        (void)datarouter_protocol_classify_frame(data, length, &layout, &chunk);
        assert(datarouter_protocol_chunk_metadata(data, length, &chunk) == DATAROUTER_OK);
        size_t written = 0;
        assert(datarouter_protocol_strip_chunk(data, length, output, sizeof(output), &written) == DATAROUTER_OK);
        assert(written <= length);
        uint64_t id; uint16_t index;
        (void)datarouter_protocol_ack_fields(data, length, &id, &index);
    }
    puts("100000 deterministic protocol inputs passed");
    return 0;
}
