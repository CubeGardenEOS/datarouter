/**
 * @file datarouter_protocol.h
 * @brief Datarouter IPC protocol codec.
 *
 * This header declares the binary message format used for inter-process
 * communication over the datarouter shared-memory transport. It handles
 * all framing (8-byte header, TLV var_data, chunking, ack flow control)
 * so that applications only deal with business-level concepts.
 *
 * Wire format:
 *   [1B type][1B flags][2B var_data_len LE][4B body_len LE][var_data...][body...]
 *
 * var_data is TLV-encoded:
 *   [1B tag][2B length LE][value bytes...]
 *
 * Business messages carry:
 *   - Module (TAG_MODULE): namespace like "admin", "agent", "public"
 *   - Action (TAG_ACTION): function name like "auth.login"
 *   - Payload (body): raw bytes, opaque to the protocol layer
 *
 * FFI safety contract:
 *   - Every non-NULL datarouter_message pointer must be a live handle returned
 *     by this API. Callers must prevent mutation/free from racing any access.
 *   - For every pointer/length pair, a non-zero length requires a non-NULL
 *     pointer valid for exactly that many readable or writable bytes. NULL is
 *     permitted when the length is zero. Lengths must not exceed PTRDIFF_MAX.
 *   - Required string pointers must reference readable NUL-terminated strings
 *     contained in one allocated object, with the terminator no farther than
 *     PTRDIFF_MAX bytes away. Optional strings may be NULL where documented.
 *   - Rust panics are contained at the ABI boundary: status functions return
 *     DATAROUTER_ERR_INTERNAL, constructors return NULL, and void/accessor
 *     functions return without effect or use their documented safe default.
 *     This containment cannot make an invalid or dangling pointer safe.
 */

#ifndef DATAROUTER_PROTOCOL_H
#define DATAROUTER_PROTOCOL_H

#include "datarouter.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── Message Types ──────────────────────────────────────────────────────── */

#define DATAROUTER_MSG_BUSINESS_REQUEST    0x07
#define DATAROUTER_MSG_BUSINESS_RESPONSE   0x8C
#define DATAROUTER_MSG_CHUNK_ACK           0x05
#define DATAROUTER_MSG_CHUNK_ERROR         0x06
#define DATAROUTER_MSG_ROUTES_REFRESH      0x85
#define DATAROUTER_MSG_CACHE_INVALIDATE    0x87
#define DATAROUTER_MSG_AGENT_REGISTRY      0x88
#define DATAROUTER_MSG_AGENT_REQUEST       0x89
#define DATAROUTER_MSG_AGENT_EVENT         0x8A
#define DATAROUTER_MSG_CONTROL             0x8B
#define DATAROUTER_MSG_LOG_NOTICE          0x8D

/* ── TLV Tags ───────────────────────────────────────────────────────────── */

#define DATAROUTER_TAG_CONNECTION_ID   0x04
#define DATAROUTER_TAG_REQUEST_ID      0x05
#define DATAROUTER_TAG_STATUS          0x06
#define DATAROUTER_TAG_IS_TEXT         0x07
#define DATAROUTER_TAG_CHUNK_ID        0x08
#define DATAROUTER_TAG_CHUNK_INDEX     0x09
#define DATAROUTER_TAG_CHUNK_COUNT     0x0A
#define DATAROUTER_TAG_CONTEXT         0x0D
#define DATAROUTER_TAG_JSON_DATA       0x0E
#define DATAROUTER_TAG_MODULE          0x0F
#define DATAROUTER_TAG_ACTION          0x10
#define DATAROUTER_TAG_STREAMING       0x11
#define DATAROUTER_TAG_WORKFLOW_ID     0x12
#define DATAROUTER_TAG_TASK_UUID       0x13
#define DATAROUTER_TAG_STEP_ID         0x14
#define DATAROUTER_TAG_ACTOR_UUID      0x15
#define DATAROUTER_TAG_TRACE_ID        0x16

#define DATAROUTER_MAX_APP_PAYLOAD \
    (DATAROUTER_RING_CAPACITY - DATAROUTER_FRAME_HEADER_SIZE)

/* ── Single Source of Truth: Wire / TLV validation ──────────────────────── */
/* These C functions are the canonical protocol validation rules. The Rust
 * frontend delegates to them via FFI and must not carry a second copy of the
 * same rules. See src/validation.c for the implementation. */

typedef struct {
    uint8_t  message_type;
    uint8_t  flags;
    uint16_t var_data_length; /* declared length (LE, header[2..4]) */
    uint32_t body_length;     /* declared length (LE, header[4..8]) */
    uint32_t total_length;    /* 8 + var_data_length + body_length */
} datarouter_frame_layout;

/**
 * Validate a TLV block (3-byte header + in-bounds value per entry, exact
 * consumption, no length overflow).
 * @param data  TLV bytes; may be NULL only when len is 0.
 * @param len   Byte count.
 * @return 1 if valid, 0 if malformed. An empty block is valid.
 */
int datarouter_protocol_validate_tlvs(const uint8_t *data, size_t len);

/**
 * Parse and validate an 8-byte frame header against the actual buffer length.
 * Validation is exact (buf_len == 8 + var_data + body).
 * @param data     Frame bytes; must be readable for buf_len bytes.
 * @param buf_len  Actual readable frame byte count.
 * @param out      Non-NULL writable datarouter_frame_layout.
 * @return DATAROUTER_OK on success (fills *out), DATAROUTER_ERR_PROTOCOL on
 * malformed/mismatched lengths, DATAROUTER_ERR_INVALID_ARGUMENT if out is NULL.
 */
int datarouter_protocol_frame_layout(const uint8_t *data, size_t buf_len,
                                     datarouter_frame_layout *out);

/* Chunk-classification outcome of a single TLV pass. */
typedef struct {
    int      present;    /* 0=none 1=complete(all three) -1=incomplete -2=invalid */
    uint8_t  has_chunk;  /* 1 if any chunk tag present */
    uint64_t chunk_id;
    uint32_t chunk_index;
    uint32_t chunk_count;
    size_t   common_len; /* bytes strip_chunk would retain when present==1 */
} datarouter_chunk_meta;

/**
 * Single-pass TLV validate + classify (mirrors Rust `parse_chunk_metadata`).
 * Always returns DATAROUTER_OK; out->present carries the outcome
 * (0 none / 1 complete / -1 incomplete / -2 malformed or invalid).
 */
int datarouter_protocol_chunk_metadata(const uint8_t *data, size_t len,
                                       datarouter_chunk_meta *out);

/**
 * Single-pass strip of chunk TLV fields (mirrors Rust `strip_chunk_metadata`).
 * Returns DATAROUTER_OK, sets *out_len; copies when cap is large enough.
 */
int datarouter_protocol_strip_chunk(const uint8_t *data, size_t len,
                                    uint8_t *out, size_t cap, size_t *out_len);

/**
 * Extract (chunk_id, chunk_index) from a chunk-control message's var_data
 * (mirrors Rust `chunk_control_fields_from_var_data`).
 * @return DATAROUTER_OK (fills *id,*index) or DATAROUTER_ERR_NOT_FOUND.
 */
int datarouter_protocol_ack_fields(const uint8_t *data, size_t len,
                                   uint64_t *id, uint16_t *index);

/**
 * Single-pass frame classification for the receive hot path: exact
 * frame-header/length validation + var_data chunk-metadata TLV classify in
 * one C call (one FFI crossing). Copy-neutral (reads borrowed bytes only).
 * @return DATAROUTER_OK (fills *layout and *chunk) or
 * DATAROUTER_ERR_PROTOCOL on header/length mismatch.
 */
int datarouter_protocol_classify_frame(const uint8_t *data, size_t buf_len,
                                       datarouter_frame_layout *layout,
                                       datarouter_chunk_meta *chunk);

/* ── Opaque Types ───────────────────────────────────────────────────────── */

typedef struct datarouter_message datarouter_message;

/* ── Message Lifecycle ──────────────────────────────────────────────────── */

/**
 * Create a new message with the given type.
 * @param message_type  One of DATAROUTER_MSG_* constants.
 * @return Newly allocated message, or NULL on failure. Free with datarouter_msg_free.
 * The returned handle is exclusively owned by the caller.
 */
datarouter_message *datarouter_msg_new(uint8_t message_type);

/**
 * Free a message. Safe to call with NULL. A non-NULL pointer must be a live,
 * exclusively owned handle returned by this API and must be freed exactly once.
 */
void datarouter_msg_free(datarouter_message *msg);

/**
 * Set the body of a message (replaces existing).
 * @param msg   Live message with no concurrent readers, writers, or free.
 * @param data  Body bytes. May be NULL if len is 0.
 * @param len   Body length in bytes; must not exceed PTRDIFF_MAX.
 * Invalid arguments are ignored because this compatibility API returns void.
 */
void datarouter_msg_set_body(datarouter_message *msg, const void *data, size_t len);

/**
 * Set the var_data of a message (replaces existing).
 * @param msg   Live message with no concurrent readers, writers, or free.
 * @param data  Raw TLV bytes. May be NULL if len is 0.
 * @param len   var_data length in bytes; must not exceed PTRDIFF_MAX.
 * Invalid arguments are ignored. Malformed TLV data is rejected by pack.
 */
void datarouter_msg_set_var_data(datarouter_message *msg, const void *data, size_t len);

/* ── TLV Builders (append to var_data) ──────────────────────────────────── */

/**
 * Append a string TLV field to the message's var_data.
 * @param msg    Live message with no concurrent readers, writers, or free.
 * @param tag    TLV tag constant (DATAROUTER_TAG_*).
 * @param value  Non-NULL, NUL-terminated UTF-8 string readable for this call.
 * Invalid UTF-8 or invalid pointers are ignored. Values too long for the u16
 * TLV length are marked invalid so a subsequent datarouter_msg_pack fails.
 */
void datarouter_tlv_put_string(datarouter_message *msg, uint8_t tag, const char *value);

/**
 * Append a bytes TLV field to the message's var_data.
 * @param msg   Live message with no concurrent readers, writers, or free.
 * @param data  Bytes to append; may be NULL only when len is zero.
 * @param len   Byte count; must not exceed PTRDIFF_MAX.
 * An empty value is ignored. Values too long for the u16 TLV length are
 * marked invalid so a subsequent datarouter_msg_pack fails.
 */
void datarouter_tlv_put_bytes(datarouter_message *msg, uint8_t tag, const void *data, size_t len);

/**
 * Append a u16 LE TLV field to the message's var_data.
 * msg must be live and exclusively accessible for this call.
 */
void datarouter_tlv_put_u16(datarouter_message *msg, uint8_t tag, uint16_t value);

/**
 * Append a single byte TLV field to the message's var_data.
 * msg must be live and exclusively accessible for this call.
 */
void datarouter_tlv_put_byte(datarouter_message *msg, uint8_t tag, uint8_t value);

/* ── TLV Readers ────────────────────────────────────────────────────────── */

/**
 * Read a string TLV field from the message's var_data.
 * msg must remain live and unmodified for this call.
 * @param tag     TLV tag to look up.
 * @param buf     Output buffer; may be NULL only when buf_len is zero.
 * @param buf_len Writable size of buf, at most PTRDIFF_MAX.
 * @return Number of bytes written (excluding the terminator), or -1 if the
 * tag/arguments are invalid. A valid non-empty buf is always NUL-terminated.
 */
int datarouter_tlv_read_string(const datarouter_message *msg, uint8_t tag, char *buf, size_t buf_len);

/**
 * Read a bytes TLV field from the message's var_data.
 * msg must remain live and unmodified for this call. buf and out_len must not
 * overlap the message or each other.
 * @param tag     TLV tag to look up.
 * @param buf     Output buffer; may be NULL only when buf_len is zero. Use
 *                (NULL, 0) to query the length without copying.
 * @param buf_len Writable size of buf, at most PTRDIFF_MAX.
 * @param out_len Non-NULL writable pointer, set to the actual data length on
 *                success. A short buffer is not written and still returns OK.
 * @return DATAROUTER_OK on success, DATAROUTER_ERR_NOT_FOUND if tag absent,
 * DATAROUTER_ERR_INVALID_ARGUMENT for an invalid pointer/length pair, or
 * DATAROUTER_ERR_INTERNAL if an internal panic was contained.
 */
int datarouter_tlv_read_bytes(const datarouter_message *msg, uint8_t tag, void *buf, size_t buf_len, size_t *out_len);

/**
 * Read a u16 LE TLV field.
 * msg must remain live and unmodified. out must be a non-NULL, non-overlapping
 * writable pointer that is not concurrently accessed.
 * @return DATAROUTER_OK on success, DATAROUTER_ERR_NOT_FOUND if tag absent,
 * DATAROUTER_ERR_INVALID_ARGUMENT for invalid pointers, or
 * DATAROUTER_ERR_INTERNAL if an internal panic was contained.
 */
int datarouter_tlv_read_u16(const datarouter_message *msg, uint8_t tag, uint16_t *out);

/**
 * Read a single byte TLV field.
 * msg must remain live and unmodified. out must be a non-NULL, non-overlapping
 * writable pointer that is not concurrently accessed.
 * @return DATAROUTER_OK on success, DATAROUTER_ERR_NOT_FOUND if tag absent,
 * DATAROUTER_ERR_INVALID_ARGUMENT for invalid pointers, or
 * DATAROUTER_ERR_INTERNAL if an internal panic was contained.
 */
int datarouter_tlv_read_byte(const datarouter_message *msg, uint8_t tag, uint8_t *out);

/* ── Wire Format ────────────────────────────────────────────────────────── */

/**
 * Pack a message into wire format.
 * msg must remain live and unmodified. Output objects must not overlap msg or
 * each other.
 * @param out_buf      Output buffer; may be NULL only when out_capacity is 0.
 * @param out_capacity Writable bytes in out_buf, at most PTRDIFF_MAX.
 * @param out_len      Non-NULL writable pointer. Set to the required wire size
 *                     after successful encoding, even if out_buf is too small.
 * @return DATAROUTER_OK on success, DATAROUTER_ERR_PAYLOAD_TOO_LARGE if the
 * buffer is too small, DATAROUTER_ERR_PROTOCOL for an unencodable message,
 * DATAROUTER_ERR_INVALID_ARGUMENT for invalid pointers, or
 * DATAROUTER_ERR_INTERNAL if an internal panic was contained.
 */
int datarouter_msg_pack(const datarouter_message *msg, void *out_buf, size_t out_capacity, size_t *out_len);

/**
 * Unpack wire data into a message.
 * @param data  Wire-format bytes; may be NULL only when len is zero.
 * @param len   Readable byte count, at most PTRDIFF_MAX. An empty input is a
 *              valid pointer/length pair but is rejected as a protocol error.
 * @param out   Non-NULL writable pointer, set to NULL before input validation
 *              and set to a new owned message only on success. It must not
 *              overlap data. Free a successful result with datarouter_msg_free.
 * @return DATAROUTER_OK on success, DATAROUTER_ERR_PROTOCOL for malformed
 * input, DATAROUTER_ERR_INVALID_ARGUMENT for invalid pointers, or
 * DATAROUTER_ERR_INTERNAL if an internal panic was contained.
 */
int datarouter_msg_unpack(const void *data, size_t len, datarouter_message **out);

/* ── High-level Business API ────────────────────────────────────────────── */

/**
 * Build a business request message.
 *
 * @param module         Module name (e.g. "admin").
 * @param action         Action name (e.g. "auth.login").
 * @param request_id     Request correlation ID.
 * The three parameters above must be non-NULL NUL-terminated strings readable
 * for this call.
 * @param connection_id  Optional NUL-terminated connection ID; may be NULL or
 *                       point to an empty string.
 * @param context_json   Optional context JSON; may be NULL only when
 *                       context_len is zero.
 * @param context_len    Readable context byte count, at most PTRDIFF_MAX.
 * @param payload        Payload; may be NULL only when payload_len is zero.
 * @param payload_len    Readable payload byte count, at most PTRDIFF_MAX.
 * @return Newly allocated message, or NULL on error. Free with datarouter_msg_free.
 * Inputs must not be concurrently mutated during the call.
 */
datarouter_message *datarouter_build_business_request(
    const char *module,
    const char *action,
    const char *request_id,
    const char *connection_id,
    const void *context_json,
    size_t context_len,
    const void *payload,
    size_t payload_len
);

/**
 * Build a business response message.
 *
 * @param module         Module name.
 * @param action         Action name.
 * @param request_id     Request correlation ID.
 * The three parameters above must be non-NULL NUL-terminated strings readable
 * for this call.
 * @param connection_id  Optional NUL-terminated connection ID; may be NULL or
 *                       point to an empty string.
 * @param payload        Response payload; may be NULL only when payload_len is zero.
 * @param payload_len    Readable payload byte count, at most PTRDIFF_MAX.
 * @param streaming      1 if more responses will follow, 0 if this is the final response.
 * @return Newly allocated message, or NULL on error. Free with datarouter_msg_free.
 * Inputs must not be concurrently mutated during the call.
 */
datarouter_message *datarouter_build_business_response(
    const char *module,
    const char *action,
    const char *request_id,
    const char *connection_id,
    const void *payload,
    size_t payload_len,
    uint8_t streaming
);

/* ── Field Accessors ────────────────────────────────────────────────────── */

/**
 * Return the message type, or 0 for NULL/panic. msg must remain live and
 * unmodified for this call.
 */
uint8_t datarouter_msg_type(const datarouter_message *msg);

/**
 * Return the message flags, or 0 for NULL/panic. msg must remain live and
 * unmodified for this call.
 */
uint8_t datarouter_msg_flags(const datarouter_message *msg);

/**
 * Set flags. msg may be NULL; otherwise it must be live and exclusively
 * accessible for this call. Invalid input or panic has no effect.
 */
void datarouter_msg_set_flags(datarouter_message *msg, uint8_t flags);

/**
 * Return a borrowed var_data pointer, or NULL when empty/invalid. msg must
 * remain live and unmodified for this call. The pointer is invalidated by any
 * later mutation or free; use datarouter_msg_var_data_len for its byte count.
 */
const uint8_t *datarouter_msg_var_data(const datarouter_message *msg);

/** Return var_data length, or 0 for NULL/panic. msg must remain live. */
size_t datarouter_msg_var_data_len(const datarouter_message *msg);

/**
 * Return a borrowed body pointer, or NULL when empty/invalid. msg must remain
 * live and unmodified for this call. The pointer is invalidated by any later
 * mutation or free; use datarouter_msg_body_len for its byte count.
 */
const uint8_t *datarouter_msg_body(const datarouter_message *msg);

/** Return body length, or 0 for NULL/panic. msg must remain live. */
size_t datarouter_msg_body_len(const datarouter_message *msg);

/* ── Constant Getters ───────────────────────────────────────────────────── */
/* These return the compile-time constant values for use from Go/Python.     */

uint8_t datarouter_tag_module(void);
uint8_t datarouter_tag_action(void);
uint8_t datarouter_tag_streaming(void);
uint8_t datarouter_tag_request_id(void);
uint8_t datarouter_tag_connection_id(void);
uint8_t datarouter_tag_context(void);
uint8_t datarouter_tag_chunk_id(void);
uint8_t datarouter_tag_chunk_index(void);
uint8_t datarouter_tag_chunk_count(void);
uint8_t datarouter_tag_status(void);
uint8_t datarouter_tag_is_text(void);
uint8_t datarouter_tag_json_data(void);
uint8_t datarouter_tag_workflow_id(void);
uint8_t datarouter_tag_task_uuid(void);
uint8_t datarouter_tag_step_id(void);
uint8_t datarouter_tag_actor_uuid(void);
uint8_t datarouter_tag_trace_id(void);

uint8_t datarouter_msg_business_request(void);
uint8_t datarouter_msg_business_response(void);
uint8_t datarouter_msg_chunk_ack(void);
uint8_t datarouter_msg_chunk_error(void);
uint8_t datarouter_msg_routes_refresh(void);
uint8_t datarouter_msg_cache_invalidate(void);
uint8_t datarouter_msg_agent_registry_update(void);
uint8_t datarouter_msg_agent_request(void);
uint8_t datarouter_msg_agent_event(void);
uint8_t datarouter_msg_control(void);
uint8_t datarouter_msg_log_notice(void);
size_t  datarouter_max_app_payload(void);

#ifdef __cplusplus
}
#endif

#endif /* DATAROUTER_PROTOCOL_H */
