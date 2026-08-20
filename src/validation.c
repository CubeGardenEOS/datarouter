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
#include <string.h>

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
/*
 * Single-pass frame classification for the receive hot path: performs the
 * exact frame-header/length validation AND the var_data chunk-metadata TLV
 * classification in one C call, so the caller needs a single FFI crossing
 * instead of (frame_layout + chunk_metadata). Copy-neutral: this only reads
 * the already-received bytes; it never copies payload.
 *
 * Returns DATAROUTER_OK and fills *layout and *chunk when the frame header is
 * well-formed and exactly matches buf_len; otherwise DATAROUTER_ERR_PROTOCOL.
 * chunk->present carries the TLV outcome as in datarouter_protocol_chunk_metadata.
 */
int datarouter_protocol_classify_frame(const uint8_t *data, size_t buf_len,
                                       datarouter_frame_layout *layout,
                                       datarouter_chunk_meta *chunk) {
    if (layout == NULL || chunk == NULL || (buf_len != 0 && data == NULL)) {
        return DATAROUTER_ERR_INVALID_ARGUMENT;
    }
    int rc = datarouter_protocol_frame_layout(data, buf_len, layout);
    if (rc != DATAROUTER_OK) {
        return rc;
    }
    /* The exact-length validation guarantees [8, 8+var_data_length) is in-bounds. */
    size_t vd_len = layout->var_data_length;
    (void)datarouter_protocol_chunk_metadata(
        data + DATAROUTER_FRAME_HEADER_SIZE, vd_len, chunk);
    return DATAROUTER_OK;
}

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

/* ---- Chunk / ACK wire codec (single source of truth, C) ----
 *
 * These mirror the prior Rust `parse_chunk_metadata`, `strip_chunk_metadata`
 * and `chunk_control_fields_from_var_data`. A single TLV pass both validates
 * and classifies, so the receive hot path no longer needs a validate+re-walk
 * (previously an extra FFI validation call plus a second Rust scan).
 */

static int tlv_chunk_tag(uint8_t tag) {
    return tag == DATAROUTER_TAG_CHUNK_ID || tag == DATAROUTER_TAG_CHUNK_INDEX ||
           tag == DATAROUTER_TAG_CHUNK_COUNT;
}

/*
 * Single-pass TLV validate + classify. Always returns DATAROUTER_OK;
 * `out->present` carries the outcome:
 *     0  valid, no chunk tags
 *     1  valid, all three chunk fields present and well-formed
 *    -1  valid, chunk tags present but incomplete (missing field(s))
 *    -2  malformed TLV, or an invalid/duplicate chunk field
 * When present == 1, `common_len` is the byte count strip_chunk would retain.
 */
int datarouter_protocol_chunk_metadata(const uint8_t *data, size_t len,
                                       datarouter_chunk_meta *out) {
    if (out == NULL) {
        return DATAROUTER_ERR_INVALID_ARGUMENT;
    }
    out->present = 0;
    out->has_chunk = 0;
    out->chunk_id = 0;
    out->chunk_index = 0;
    out->chunk_count = 0;
    out->common_len = 0;

    size_t offset = 0;
    int have_id = 0, have_index = 0, have_count = 0;
    int any_chunk = 0;
    uint64_t id = 0;
    uint16_t index = 0, count = 0;
    size_t common = 0;

    while (offset < len) {
        if (offset > SIZE_MAX - 3) {
            out->present = -2;
            return DATAROUTER_OK;
        }
        size_t header_end = offset + 3;
        if (header_end > len) {
            out->present = -2;
            return DATAROUTER_OK;
        }
        uint8_t tag = data[offset];
        uint32_t vlen = (uint32_t)data[offset + 1] | ((uint32_t)data[offset + 2] << 8);
        if (header_end > SIZE_MAX - (size_t)vlen) {
            out->present = -2;
            return DATAROUTER_OK;
        }
        size_t value_end = header_end + (size_t)vlen;
        if (value_end > len) {
            out->present = -2;
            return DATAROUTER_OK;
        }

        if (tlv_chunk_tag(tag)) {
            any_chunk = 1;
        }
        if (tag == DATAROUTER_TAG_CHUNK_ID) {
            if (have_id || vlen != 8) {
                out->present = -2;
                return DATAROUTER_OK;
            }
            have_id = 1;
            id = (uint64_t)data[header_end] | ((uint64_t)data[header_end + 1] << 8) |
                 ((uint64_t)data[header_end + 2] << 16) |
                 ((uint64_t)data[header_end + 3] << 24) |
                 ((uint64_t)data[header_end + 4] << 32) |
                 ((uint64_t)data[header_end + 5] << 40) |
                 ((uint64_t)data[header_end + 6] << 48) |
                 ((uint64_t)data[header_end + 7] << 56);
        } else if (tag == DATAROUTER_TAG_CHUNK_INDEX) {
            if (have_index || vlen != 2) {
                out->present = -2;
                return DATAROUTER_OK;
            }
            have_index = 1;
            index = (uint16_t)((uint16_t)data[header_end] | ((uint16_t)data[header_end + 1] << 8));
        } else if (tag == DATAROUTER_TAG_CHUNK_COUNT) {
            if (have_count || vlen != 2) {
                out->present = -2;
                return DATAROUTER_OK;
            }
            have_count = 1;
            count = (uint16_t)((uint16_t)data[header_end] | ((uint16_t)data[header_end + 1] << 8));
        } else {
            common += (value_end - offset);
        }
        offset = value_end;
    }

    out->has_chunk = (uint8_t)(any_chunk ? 1 : 0);
    if (!any_chunk) {
        out->present = 0;
        out->common_len = len;
        return DATAROUTER_OK;
    }
    out->common_len = common;
    if (have_id && have_index && have_count) {
        out->present = 1;
        out->chunk_id = id;
        out->chunk_index = index;
        out->chunk_count = count;
    } else {
        out->present = -1;
    }
    return DATAROUTER_OK;
}

/*
 * Single-pass strip of chunk TLV fields. Mirrors Rust `strip_chunk_metadata`:
 * when the block is a complete chunk payload (present==1) copies only the
 * non-chunk TLVs; otherwise (malformed, incomplete, or no chunk metadata)
 * returns the original bytes unchanged (copies all `len` bytes).
 *
 * Returns DATAROUTER_OK and sets *out_len (and copies when cap is large
 * enough); the copied byte count in *out_len is the truth even if cap is
 * too small (caller reallocates). DATAROUTER_ERR_INVALID_ARGUMENT for NULL
 * pointers when len is non-zero.
 */
int datarouter_protocol_strip_chunk(const uint8_t *data, size_t len,
                                    uint8_t *out, size_t cap, size_t *out_len) {
    if (out_len == NULL || (len != 0 && data == NULL)) {
        return DATAROUTER_ERR_INVALID_ARGUMENT;
    }
    datarouter_chunk_meta meta;
    (void)datarouter_protocol_chunk_metadata(data, len, &meta);

    size_t write_len;
    const uint8_t *src = data;
    size_t written = 0;
    size_t offset = 0;

    if (meta.present == 1) {
        /* copy only non-chunk TLVs */
        while (offset < len) {
            size_t header_end = offset + 3;
            uint32_t vlen = (uint32_t)data[offset + 1] | ((uint32_t)data[offset + 2] << 8);
            size_t value_end = header_end + (size_t)vlen;
            uint8_t tag = data[offset];
            if (!tlv_chunk_tag(tag)) {
                size_t emit = value_end - offset;
                if (written + emit <= cap && out != NULL) {
                    memcpy(out + written, data + offset, emit);
                }
                written += emit;
            }
            offset = value_end;
        }
        write_len = written;
    } else {
        /* return original unchanged */
        write_len = len;
        if (cap >= len && out != NULL && len != 0) {
            memcpy(out, data, len);
        }
        src = data;
    }
    *out_len = write_len;
    (void)src;
    return DATAROUTER_OK;
}

/*
 * Extract the (chunk_id, chunk_index) pair from a chunk-control message's
 * var_data (chunk.ack / chunk.error). Mirrors Rust
 * `chunk_control_fields_from_var_data`: validates, rejects duplicates / wrong
 * lengths / missing fields / malformed TLV.
 *
 * Returns DATAROUTER_OK and fills *id, *index on success; otherwise
 * DATAROUTER_ERR_NOT_FOUND (or INVALID_ARGUMENT for NULL pointers).
 */
int datarouter_protocol_ack_fields(const uint8_t *data, size_t len,
                                   uint64_t *id, uint16_t *index) {
    if (id == NULL || index == NULL || (len != 0 && data == NULL)) {
        return DATAROUTER_ERR_INVALID_ARGUMENT;
    }
    size_t offset = 0;
    int have_id = 0, have_index = 0;
    uint64_t got_id = 0;
    uint16_t got_index = 0;
    while (offset < len) {
        if (offset > SIZE_MAX - 3) {
            return DATAROUTER_ERR_NOT_FOUND;
        }
        size_t header_end = offset + 3;
        if (header_end > len) {
            return DATAROUTER_ERR_NOT_FOUND;
        }
        uint8_t tag = data[offset];
        uint32_t vlen = (uint32_t)data[offset + 1] | ((uint32_t)data[offset + 2] << 8);
        if (header_end > SIZE_MAX - (size_t)vlen) {
            return DATAROUTER_ERR_NOT_FOUND;
        }
        size_t value_end = header_end + (size_t)vlen;
        if (value_end > len) {
            return DATAROUTER_ERR_NOT_FOUND;
        }
        if (tag == DATAROUTER_TAG_CHUNK_ID) {
            if (have_id || vlen != 8) {
                return DATAROUTER_ERR_NOT_FOUND;
            }
            have_id = 1;
            got_id = (uint64_t)data[header_end] | ((uint64_t)data[header_end + 1] << 8) |
                     ((uint64_t)data[header_end + 2] << 16) |
                     ((uint64_t)data[header_end + 3] << 24) |
                     ((uint64_t)data[header_end + 4] << 32) |
                     ((uint64_t)data[header_end + 5] << 40) |
                     ((uint64_t)data[header_end + 6] << 48) |
                     ((uint64_t)data[header_end + 7] << 56);
        } else if (tag == DATAROUTER_TAG_CHUNK_INDEX) {
            if (have_index || vlen != 2) {
                return DATAROUTER_ERR_NOT_FOUND;
            }
            have_index = 1;
            got_index = (uint16_t)((uint16_t)data[header_end] | ((uint16_t)data[header_end + 1] << 8));
        }
        offset = value_end;
    }
    if (have_id && have_index) {
        *id = got_id;
        *index = got_index;
        return DATAROUTER_OK;
    }
    return DATAROUTER_ERR_NOT_FOUND;
}
