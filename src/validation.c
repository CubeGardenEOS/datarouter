/*
 * datarouter_protocol validation — single source of truth (C).
 *
 * These are the canonical protocol validation rules for the datarouter wire
 * format. The Rust frontend delegates to these functions via FFI and MUST NOT
 * carry a second, independently-maintained implementation of the same rules
 * (drift between Rust/C/Go/Python would otherwise be inevitable).
 *
 * Wire format (see include/datarouter_protocol.h):
 *   [1B type][1B flags][2B var_data_len LE][4B body_len LE][var_data][body]
 *   var_data = [1B tag][2B length LE][value bytes]...
 */

#include <stddef.h>
#include <stdint.h>

#include "datarouter.h"
#include "datarouter_protocol.h"

/*
 * Validate that `len` bytes starting at `data` form a complete, exactly-sized
 * sequence of TLV entries (3-byte header + in-bounds value, no trailing
 * partial header or value, no length overflow).
 *
 * Returns 1 if valid, 0 if malformed. An empty block (len == 0) is valid.
 * `data` may be NULL only when `len` is 0; it is never dereferenced then.
 *
 * Semantics are byte-for-byte equivalent to the prior Rust `validate_tlvs`.
 */
int datarouter_protocol_validate_tlvs(const uint8_t *data, size_t len) {
    size_t offset = 0;
    while (offset < len) {
        /* TLV offset overflow (3-byte header) */
        if (offset > SIZE_MAX - 3) {
            return 0;
        }
        size_t header_end = offset + 3;
        if (header_end > len) {
            return 0; /* truncated TLV header */
        }
        uint32_t vlen = (uint32_t)data[offset + 1] | ((uint32_t)data[offset + 2] << 8);
        /* TLV length overflow */
        if (header_end > SIZE_MAX - (size_t)vlen) {
            return 0;
        }
        size_t value_end = header_end + (size_t)vlen;
        if (value_end > len) {
            return 0; /* truncated TLV value */
        }
        offset = value_end;
    }
    return 1;
}

/*
 * Parse and validate an 8-byte datarouter frame header against the actual
 * buffer length. On success fills `*out` and returns DATAROUTER_OK.
 *
 * Validation is exact: `buf_len` must equal 8 + declared var_data_length +
 * declared body_length (no truncation, no trailing bytes), mirroring the
 * previous Rust `raw_frame_layout`/`unpack` exact-match rule.
 *
 * Returns DATAROUTER_ERR_PROTOCOL when the header is malformed or the declared
 * lengths do not match `buf_len`, and DATAROUTER_ERR_INVALID_ARGUMENT when
 * `out` is NULL. `data` is only dereferenced after the `buf_len >= 8` guard.
 */
int datarouter_protocol_frame_layout(const uint8_t *data, size_t buf_len,
                                     datarouter_frame_layout *out) {
    if (out == NULL) {
        return DATAROUTER_ERR_INVALID_ARGUMENT;
    }
    if (buf_len < DATAROUTER_FRAME_HEADER_SIZE) {
        return DATAROUTER_ERR_PROTOCOL;
    }
    uint16_t vd_len = (uint16_t)((uint16_t)data[2] | ((uint16_t)data[3] << 8));
    uint32_t b_len = (uint32_t)data[4] | ((uint32_t)data[5] << 8) |
                     ((uint32_t)data[6] << 16) | ((uint32_t)data[7] << 24);
    uint64_t total = (uint64_t)DATAROUTER_FRAME_HEADER_SIZE + (uint64_t)vd_len + (uint64_t)b_len;
    if (total != (uint64_t)buf_len) {
        return DATAROUTER_ERR_PROTOCOL;
    }
    out->message_type = data[0];
    out->flags = data[1];
    out->var_data_length = vd_len;
    out->body_length = b_len;
    out->total_length = (uint32_t)total;
    return DATAROUTER_OK;
}
