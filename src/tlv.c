/*
 * datarouter TLV builder/reader — single source of truth (C).
 *
 * These are the canonical TLV encode/decode primitives for var_data blocks
 * (`[1B tag][2B length LE][value bytes]...`). The Rust frontend
 * (`protocol.rs::VarDataBuilder` / `tlv_read_*` / `parse_business_fields`)
 * and the `datarouter_tlv_*` exports in `protocol_ffi.rs` all delegate here;
 * Rust MUST NOT carry a second, independently-maintained TLV walk.
 *
 * Copy discipline: the builder performs exactly one memcpy (value -> block);
 * the reader returns a POINTER into the block (zero-copy). Neither adds a copy
 * to the datarouter data plane.
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "datarouter_protocol.h"

/* Distinct iteration outcome codes for datarouter_tlv_next (avoid reusing the
 * DATAROUTER_ERR_* wire codes, which carry transport semantics). */
#define DATAROUTER_TLV_NEXT_DECODED 1
#define DATAROUTER_TLV_NEXT_END 0
#define DATAROUTER_TLV_NEXT_TRUNC_HEADER (-1)
#define DATAROUTER_TLV_NEXT_TRUNC_VALUE (-2)
#define DATAROUTER_TLV_NEXT_BADARG (-3)

int datarouter_tlv_append(uint8_t *buf, size_t cap, size_t len,
                          uint8_t tag, const void *data, size_t data_len,
                          size_t *out_new_len) {
    if (out_new_len == NULL ||
        (buf == NULL && (cap != 0u || len != 0u || data_len != 0u)) ||
        (data == NULL && data_len != 0u)) {
        return DATAROUTER_ERR_INVALID_ARGUMENT;
    }
    if (data_len > 0xFFFFu) {
        return DATAROUTER_ERR_PAYLOAD_TOO_LARGE;
    }
    if (len > (size_t)-1 - 3u || len + 3u > (size_t)-1 - data_len) {
        return DATAROUTER_ERR_PROTOCOL; /* arithmetic overflow */
    }
    size_t needed = len + 3u + data_len;
    if (needed > cap) {
        return DATAROUTER_ERR_FULL; /* caller must grow & retry */
    }
    uint8_t *p = buf + len;
    *p++ = tag;
    p[0] = (uint8_t)(data_len & 0xFFu);
    p[1] = (uint8_t)((data_len >> 8) & 0xFFu);
    p += 2;
    if (data_len != 0u) {
        memcpy(p, data, data_len); /* the single builder copy */
    }
    *out_new_len = needed;
    return DATAROUTER_OK;
}

int datarouter_tlv_find(const uint8_t *data, size_t data_len, uint8_t tag,
                        size_t start, const uint8_t **out_value, size_t *out_len) {
    if (out_value == NULL || out_len == NULL ||
        (data == NULL && data_len != 0u) || start > data_len) {
        return DATAROUTER_ERR_INVALID_ARGUMENT;
    }
    size_t i = 0u;
    int matched = 0;
    const uint8_t *match_value = NULL;
    size_t match_len = 0u;
    while (i < data_len) {
        if (data_len - i < 3u) {
            return DATAROUTER_ERR_PROTOCOL; /* truncated header */
        }
        uint8_t t = data[i];
        size_t l = (size_t)data[i + 1u] | ((size_t)data[i + 2u] << 8u);
        i += 3u;
        if (data_len - i < l) {
            return DATAROUTER_ERR_PROTOCOL; /* truncated value */
        }
        /* Full-scan so a malformed tail yields PROTOCOL even when the target
         * tag precedes it (exact "malformed block => not found" semantics). */
        if (!matched && i >= start && t == tag) {
            match_value = data + i;
            match_len = l;
            matched = 1;
        }
        i += l;
    }
    if (matched) {
        *out_value = match_value;
        *out_len = match_len;
        return DATAROUTER_OK;
    }
    return DATAROUTER_ERR_NOT_FOUND;
}

int datarouter_tlv_next(const uint8_t *data, size_t data_len, size_t *cursor,
                        uint8_t *out_tag, const uint8_t **out_value,
                        size_t *out_len) {
    if (data == NULL || cursor == NULL || out_tag == NULL ||
        out_value == NULL || out_len == NULL) {
        return DATAROUTER_TLV_NEXT_BADARG;
    }
    if (*cursor >= data_len) {
        return DATAROUTER_TLV_NEXT_END;
    }
    if (data_len - *cursor < 3u) {
        return DATAROUTER_TLV_NEXT_TRUNC_HEADER;
    }
    uint8_t t = data[*cursor];
    size_t l = (size_t)data[*cursor + 1u] | ((size_t)data[*cursor + 2u] << 8u);
    size_t value_off = *cursor + 3u;
    if (data_len - value_off < l) {
        return DATAROUTER_TLV_NEXT_TRUNC_VALUE;
    }
    *out_tag = t;
    *out_value = data + value_off;
    *out_len = l;
    *cursor = value_off + l;
    return DATAROUTER_TLV_NEXT_DECODED;
}
