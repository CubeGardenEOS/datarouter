//! C ABI for the datarouter protocol codec.
//!
//! These functions are exported as `extern "C"` and can be called from
//! C, Go (via CGo), Python (via ctypes), or any language with FFI support.

use std::ffi::CStr;
use std::os::raw::{c_char, c_int, c_void};
use std::panic::{self, AssertUnwindSafe};
use std::ptr;
use std::slice;

use crate::protocol;

fn ffi_or<T>(fallback: T, operation: impl FnOnce() -> T) -> T {
    panic::catch_unwind(AssertUnwindSafe(operation)).unwrap_or(fallback)
}

fn const_buffer_is_valid<T>(data: *const T, len: usize) -> bool {
    len <= isize::MAX as usize && (len == 0 || !data.is_null())
}

fn mut_buffer_is_valid<T>(data: *mut T, len: usize) -> bool {
    len <= isize::MAX as usize && (len == 0 || !data.is_null())
}

fn mark_tlv_error(msg: &mut datarouter_message) {
    // Preserve the legacy void builder ABI while remembering that encoding
    // must fail, even if the caller appends more valid fields afterward.
    msg.protocol_error = true;
}

fn message_has_protocol_error(msg: &protocol::Message) -> bool {
    protocol::validate_tlvs(&msg.var_data).is_err()
}

// ── Opaque Message Type ───────────────────────────────────────────────────

/// Opaque message handle for C consumers.
#[repr(C)]
pub struct datarouter_message {
    inner: protocol::Message,
    protocol_error: bool,
}

// ── Message Lifecycle ─────────────────────────────────────────────────────

/// Create a new message with the given type. Returns NULL on failure.
///
/// # Safety
/// The returned handle is owned by the caller and must be released exactly
/// once with [`datarouter_msg_free`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn datarouter_msg_new(message_type: u8) -> *mut datarouter_message {
    ffi_or(ptr::null_mut(), || {
        let msg = Box::new(datarouter_message {
            inner: protocol::Message::new(message_type),
            protocol_error: false,
        });
        Box::into_raw(msg)
    })
}

/// Free a message. Safe to call with NULL.
///
/// # Safety
/// `msg` must be NULL or a live handle returned by this API. A non-NULL handle
/// must not have been freed already, and no other thread may access it during
/// or after this call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn datarouter_msg_free(msg: *mut datarouter_message) {
    ffi_or((), || {
        if !msg.is_null() {
            drop(unsafe { Box::from_raw(msg) });
        }
    });
}

/// Set the body of a message.
///
/// # Safety
/// `msg` must be a live, exclusively accessible message handle. When `len` is
/// non-zero, `data` must be non-NULL and readable for `len` bytes. `data` may
/// be NULL when `len` is zero. `len` must not exceed `isize::MAX`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn datarouter_msg_set_body(
    msg: *mut datarouter_message,
    data: *const c_void,
    len: usize,
) {
    ffi_or((), || {
        if msg.is_null() || !const_buffer_is_valid(data, len) {
            return;
        }
        let msg = unsafe { &mut (*msg).inner };
        msg.body = if len == 0 {
            Vec::new()
        } else {
            unsafe { slice::from_raw_parts(data.cast::<u8>(), len) }.to_vec()
        };
    });
}

/// Set the var_data of a message (replaces existing).
///
/// # Safety
/// `msg` must be a live, exclusively accessible message handle. When `len` is
/// non-zero, `data` must be non-NULL and readable for `len` bytes. `data` may
/// be NULL when `len` is zero. `len` must not exceed `isize::MAX`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn datarouter_msg_set_var_data(
    msg: *mut datarouter_message,
    data: *const c_void,
    len: usize,
) {
    ffi_or((), || {
        if msg.is_null() || !const_buffer_is_valid(data, len) {
            return;
        }
        let msg = unsafe { &mut *msg };
        msg.inner.var_data = if len == 0 {
            Vec::new()
        } else {
            unsafe { slice::from_raw_parts(data.cast::<u8>(), len) }.to_vec()
        };
        msg.protocol_error = false;
    });
}

// ── TLV Builders ──────────────────────────────────────────────────────────

/// Append a string TLV field to the message's var_data.
///
/// # Safety
/// `msg` must be a live, exclusively accessible message handle. `value` must
/// point to a NUL-terminated byte string within one allocated object, with the
/// terminator at most `isize::MAX` bytes away, that remains readable for this
/// call. It must contain valid UTF-8. No mutation or free of `msg` may race this
/// call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn datarouter_tlv_put_string(
    msg: *mut datarouter_message,
    tag: u8,
    value: *const c_char,
) {
    ffi_or((), || {
        if msg.is_null() || value.is_null() {
            return;
        }
        let Ok(s) = unsafe { CStr::from_ptr(value) }.to_str() else {
            return;
        };
        let msg = unsafe { &mut *msg };
        if s.len() > u16::MAX as usize {
            mark_tlv_error(msg);
            return;
        }
        let mut vd = protocol::VarDataBuilder::new();
        vd.put_string(tag, s);
        msg.inner.var_data.extend_from_slice(&vd.build());
    });
}

/// Append a bytes TLV field to the message's var_data.
///
/// # Safety
/// `msg` must be a live, exclusively accessible message handle. When `len` is
/// non-zero, `data` must be non-NULL and readable for `len` bytes. `data` may
/// be NULL when `len` is zero. `len` must not exceed `isize::MAX`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn datarouter_tlv_put_bytes(
    msg: *mut datarouter_message,
    tag: u8,
    data: *const c_void,
    len: usize,
) {
    ffi_or((), || {
        if msg.is_null() || !const_buffer_is_valid(data, len) || len == 0 {
            return;
        }
        let msg = unsafe { &mut *msg };
        if len > u16::MAX as usize {
            mark_tlv_error(msg);
            return;
        }
        let mut vd = protocol::VarDataBuilder::new();
        vd.put_bytes(tag, unsafe {
            slice::from_raw_parts(data.cast::<u8>(), len)
        });
        msg.inner.var_data.extend_from_slice(&vd.build());
    });
}

/// Append a u16 LE TLV field to the message's var_data.
///
/// # Safety
/// `msg` must be a live, exclusively accessible message handle. No other
/// thread may read, mutate, or free it during this call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn datarouter_tlv_put_u16(msg: *mut datarouter_message, tag: u8, value: u16) {
    ffi_or((), || {
        if msg.is_null() {
            return;
        }
        let msg = unsafe { &mut (*msg).inner };
        let mut vd = protocol::VarDataBuilder::new();
        vd.put_u16(tag, value);
        msg.var_data.extend_from_slice(&vd.build());
    });
}

/// Append a single byte TLV field to the message's var_data.
///
/// # Safety
/// `msg` must be a live, exclusively accessible message handle. No other
/// thread may read, mutate, or free it during this call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn datarouter_tlv_put_byte(msg: *mut datarouter_message, tag: u8, value: u8) {
    ffi_or((), || {
        if msg.is_null() {
            return;
        }
        let msg = unsafe { &mut (*msg).inner };
        let mut vd = protocol::VarDataBuilder::new();
        vd.put_byte(tag, value);
        msg.var_data.extend_from_slice(&vd.build());
    });
}

// ── TLV Readers ───────────────────────────────────────────────────────────

/// Read a string TLV field. Returns the number of bytes written to buf
/// (excluding null terminator), or -1 if not found or invalid. A valid,
/// non-empty output buffer is always null-terminated.
///
/// # Safety
/// `msg` must be a live message handle that is not concurrently mutated or
/// freed. When `buf_len` is non-zero, `buf` must be non-NULL and writable for
/// `buf_len` bytes. `buf` may be NULL when `buf_len` is zero. `buf_len` must
/// not exceed `isize::MAX`, and the output must not overlap the message.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn datarouter_tlv_read_string(
    msg: *const datarouter_message,
    tag: u8,
    buf: *mut c_char,
    buf_len: usize,
) -> c_int {
    ffi_or(-1, || {
        if msg.is_null() || !mut_buffer_is_valid(buf, buf_len) || buf_len == 0 {
            return -1;
        }
        unsafe { *buf = 0 };
        let msg = unsafe { &(*msg).inner };
        match protocol::tlv_read_string(&msg.var_data, tag) {
            Some(s) => {
                let bytes = s.as_bytes();
                let copy_len = bytes.len().min(buf_len - 1);
                unsafe {
                    ptr::copy_nonoverlapping(bytes.as_ptr(), buf.cast::<u8>(), copy_len);
                    *buf.add(copy_len) = 0;
                }
                copy_len as c_int
            }
            None => -1,
        }
    })
}

/// Read a bytes TLV field. Returns DATAROUTER_OK on success, error code otherwise.
/// `out_len` is set to the actual data length. For a `(NULL, 0)` length query
/// or a buffer that is too small, `out_len` is still set but data is not copied.
///
/// # Safety
/// `msg` must be a live message handle that is not concurrently mutated or
/// freed. `out_len` must be non-NULL, writable, and must not overlap `msg`.
/// When `buf_len` is non-zero, `buf` must be non-NULL and writable for
/// `buf_len` bytes; it may be NULL when `buf_len` is zero. `buf_len` must not
/// exceed `isize::MAX`, and `buf` must not overlap the message or `out_len`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn datarouter_tlv_read_bytes(
    msg: *const datarouter_message,
    tag: u8,
    buf: *mut c_void,
    buf_len: usize,
    out_len: *mut usize,
) -> c_int {
    ffi_or(crate::DATAROUTER_ERR_INTERNAL, || {
        if msg.is_null() || out_len.is_null() {
            return crate::DATAROUTER_ERR_INVALID_ARGUMENT;
        }
        unsafe { *out_len = 0 };
        if !mut_buffer_is_valid(buf, buf_len) {
            return crate::DATAROUTER_ERR_INVALID_ARGUMENT;
        }
        let msg = unsafe { &(*msg).inner };
        match protocol::tlv_read_bytes(&msg.var_data, tag) {
            Some(data) => {
                unsafe { *out_len = data.len() };
                if !data.is_empty() && buf_len >= data.len() {
                    unsafe {
                        ptr::copy_nonoverlapping(data.as_ptr(), buf.cast::<u8>(), data.len())
                    };
                }
                crate::DATAROUTER_OK
            }
            None => crate::DATAROUTER_ERR_NOT_FOUND,
        }
    })
}

/// Read a u16 LE TLV field. Returns DATAROUTER_OK on success.
///
/// # Safety
/// `msg` must be a live message handle that is not concurrently mutated or
/// freed. `out` must be non-NULL and writable for a `u16`, must not overlap
/// `msg`, and must not be concurrently accessed for the duration of this call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn datarouter_tlv_read_u16(
    msg: *const datarouter_message,
    tag: u8,
    out: *mut u16,
) -> c_int {
    ffi_or(crate::DATAROUTER_ERR_INTERNAL, || {
        if msg.is_null() || out.is_null() {
            return crate::DATAROUTER_ERR_INVALID_ARGUMENT;
        }
        let msg = unsafe { &(*msg).inner };
        match protocol::tlv_read_u16(&msg.var_data, tag) {
            Some(v) => {
                unsafe { *out = v };
                crate::DATAROUTER_OK
            }
            None => crate::DATAROUTER_ERR_NOT_FOUND,
        }
    })
}

/// Read a single byte TLV field. Returns DATAROUTER_OK on success.
///
/// # Safety
/// `msg` must be a live message handle that is not concurrently mutated or
/// freed. `out` must be non-NULL and writable for one byte, must not overlap
/// `msg`, and must not be concurrently accessed for the duration of this call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn datarouter_tlv_read_byte(
    msg: *const datarouter_message,
    tag: u8,
    out: *mut u8,
) -> c_int {
    ffi_or(crate::DATAROUTER_ERR_INTERNAL, || {
        if msg.is_null() || out.is_null() {
            return crate::DATAROUTER_ERR_INVALID_ARGUMENT;
        }
        let msg = unsafe { &(*msg).inner };
        match protocol::tlv_read_byte(&msg.var_data, tag) {
            Some(v) => {
                unsafe { *out = v };
                crate::DATAROUTER_OK
            }
            None => crate::DATAROUTER_ERR_NOT_FOUND,
        }
    })
}

// ── Wire Format ───────────────────────────────────────────────────────────

/// Pack a message into wire format.
///
/// On success, `out_len` is the number of bytes written. When the buffer is
/// too small, `out_len` is still set to the required capacity and
/// `DATAROUTER_ERR_PAYLOAD_TOO_LARGE` is returned without copying any bytes.
///
/// # Safety
/// `msg` must be a live message handle that is not concurrently mutated or
/// freed. `out_len` must be non-NULL and writable. When `out_capacity` is
/// non-zero, `out_buf` must be non-NULL and writable for `out_capacity` bytes;
/// it may be NULL when the capacity is zero. `out_capacity` must not exceed
/// `isize::MAX`. The output objects must not overlap each other or `msg`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn datarouter_msg_pack(
    msg: *const datarouter_message,
    out_buf: *mut c_void,
    out_capacity: usize,
    out_len: *mut usize,
) -> c_int {
    ffi_or(crate::DATAROUTER_ERR_INTERNAL, || {
        if out_len.is_null() {
            return crate::DATAROUTER_ERR_INVALID_ARGUMENT;
        }
        unsafe { *out_len = 0 };
        if msg.is_null() || !mut_buffer_is_valid(out_buf, out_capacity) {
            return crate::DATAROUTER_ERR_INVALID_ARGUMENT;
        }
        let msg = unsafe { &*msg };
        if msg.protocol_error {
            return crate::DATAROUTER_ERR_PROTOCOL;
        }
        match protocol::pack(&msg.inner) {
            Ok(packed) => {
                unsafe { *out_len = packed.len() };
                if packed.len() > out_capacity {
                    return crate::DATAROUTER_ERR_PAYLOAD_TOO_LARGE;
                }
                if !packed.is_empty() {
                    unsafe {
                        ptr::copy_nonoverlapping(
                            packed.as_ptr(),
                            out_buf.cast::<u8>(),
                            packed.len(),
                        )
                    };
                }
                crate::DATAROUTER_OK
            }
            Err(_) => crate::DATAROUTER_ERR_PROTOCOL,
        }
    })
}

/// Unpack wire data into a newly allocated message.
///
/// `*out` is set to NULL before the input is validated and remains NULL on
/// every error. The caller must free a successful result with
/// [`datarouter_msg_free`].
///
/// # Safety
/// `out` must be non-NULL and writable for one message pointer. When `len` is
/// non-zero, `data` must be non-NULL and readable for `len` bytes; it may be
/// NULL when `len` is zero. `len` must not exceed `isize::MAX`. `out` must not
/// overlap `data` and neither object may be concurrently mutated.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn datarouter_msg_unpack(
    data: *const c_void,
    len: usize,
    out: *mut *mut datarouter_message,
) -> c_int {
    ffi_or(crate::DATAROUTER_ERR_INTERNAL, || {
        if out.is_null() {
            return crate::DATAROUTER_ERR_INVALID_ARGUMENT;
        }
        unsafe { *out = ptr::null_mut() };
        if !const_buffer_is_valid(data, len) {
            return crate::DATAROUTER_ERR_INVALID_ARGUMENT;
        }
        let wire = if len == 0 {
            &[]
        } else {
            unsafe { slice::from_raw_parts(data.cast::<u8>(), len) }
        };
        match protocol::unpack(wire) {
            Ok(msg) => {
                let boxed = Box::new(datarouter_message {
                    inner: msg,
                    protocol_error: false,
                });
                unsafe { *out = Box::into_raw(boxed) };
                crate::DATAROUTER_OK
            }
            Err(_) => crate::DATAROUTER_ERR_PROTOCOL,
        }
    })
}

// ── High-level Business API ───────────────────────────────────────────────

/// Build a business request message. Returns a newly allocated message.
/// The caller must free with datarouter_msg_free.
///
/// Parameters:
/// - module: module name (e.g. "admin")
/// - action: action name (e.g. "auth.login")
/// - request_id: request correlation ID
/// - connection_id: connection ID (may be empty string)
/// - context_json: optional context JSON bytes (may be NULL)
/// - context_len: length of context_json
/// - payload: business payload bytes
/// - payload_len: length of payload
///
/// # Safety
/// `module`, `action`, and `request_id` must point to NUL-terminated strings;
/// `connection_id` may be NULL or must point to a NUL-terminated string. Each
/// string must occupy one allocated object, terminate within `isize::MAX`
/// bytes, and remain readable for this call. For each byte pointer/length pair,
/// a non-zero length requires a non-NULL pointer readable for that many bytes,
/// while a zero length permits NULL. Lengths must not exceed `isize::MAX`. The
/// inputs must not be concurrently mutated.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn datarouter_build_business_request(
    module: *const c_char,
    action: *const c_char,
    request_id: *const c_char,
    connection_id: *const c_char,
    context_json: *const c_void,
    context_len: usize,
    payload: *const c_void,
    payload_len: usize,
) -> *mut datarouter_message {
    ffi_or(ptr::null_mut(), || {
        if module.is_null()
            || action.is_null()
            || request_id.is_null()
            || !const_buffer_is_valid(context_json, context_len)
            || !const_buffer_is_valid(payload, payload_len)
        {
            return ptr::null_mut();
        }
        let module = unsafe { CStr::from_ptr(module) }.to_string_lossy();
        let action = unsafe { CStr::from_ptr(action) }.to_string_lossy();
        let request_id = unsafe { CStr::from_ptr(request_id) }.to_string_lossy();
        let connection_id = if connection_id.is_null() {
            String::new()
        } else {
            unsafe { CStr::from_ptr(connection_id) }
                .to_string_lossy()
                .into_owned()
        };
        let context = if context_len == 0 {
            None
        } else {
            Some(unsafe { slice::from_raw_parts(context_json.cast::<u8>(), context_len) })
        };
        let payload = if payload_len == 0 {
            &[]
        } else {
            unsafe { slice::from_raw_parts(payload.cast::<u8>(), payload_len) }
        };

        let msg = protocol::build_business_request(
            &module,
            &action,
            &request_id,
            &connection_id,
            context,
            payload,
        );
        let protocol_error = message_has_protocol_error(&msg);
        let boxed = Box::new(datarouter_message {
            inner: msg,
            protocol_error,
        });
        Box::into_raw(boxed)
    })
}

/// Build a business response message. Returns a newly allocated message.
///
/// # Safety
/// `module`, `action`, and `request_id` must point to NUL-terminated strings;
/// `connection_id` may be NULL or must point to a NUL-terminated string. Each
/// string must occupy one allocated object, terminate within `isize::MAX`
/// bytes, and remain readable for this call. When `payload_len` is non-zero,
/// `payload` must be non-NULL and readable for that many bytes; it may be NULL
/// when the length is zero. `payload_len` must not exceed `isize::MAX`. The
/// inputs must not be concurrently mutated.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn datarouter_build_business_response(
    module: *const c_char,
    action: *const c_char,
    request_id: *const c_char,
    connection_id: *const c_char,
    payload: *const c_void,
    payload_len: usize,
    streaming: u8,
) -> *mut datarouter_message {
    ffi_or(ptr::null_mut(), || {
        if module.is_null()
            || action.is_null()
            || request_id.is_null()
            || !const_buffer_is_valid(payload, payload_len)
        {
            return ptr::null_mut();
        }
        let module = unsafe { CStr::from_ptr(module) }.to_string_lossy();
        let action = unsafe { CStr::from_ptr(action) }.to_string_lossy();
        let request_id = unsafe { CStr::from_ptr(request_id) }.to_string_lossy();
        let connection_id = if connection_id.is_null() {
            String::new()
        } else {
            unsafe { CStr::from_ptr(connection_id) }
                .to_string_lossy()
                .into_owned()
        };
        let payload = if payload_len == 0 {
            &[]
        } else {
            unsafe { slice::from_raw_parts(payload.cast::<u8>(), payload_len) }
        };

        let msg = protocol::build_business_response(
            &module,
            &action,
            &request_id,
            &connection_id,
            payload,
            streaming != 0,
        );
        let protocol_error = message_has_protocol_error(&msg);
        let boxed = Box::new(datarouter_message {
            inner: msg,
            protocol_error,
        });
        Box::into_raw(boxed)
    })
}

// ── Field Accessors ───────────────────────────────────────────────────────

/// Get the message type.
///
/// # Safety
/// `msg` must be NULL or a live message handle that is not concurrently
/// mutated or freed. NULL and panic both produce zero.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn datarouter_msg_type(msg: *const datarouter_message) -> u8 {
    ffi_or(0, || {
        if msg.is_null() {
            0
        } else {
            unsafe { (*msg).inner.message_type }
        }
    })
}

/// Get the message flags.
///
/// # Safety
/// `msg` must be NULL or a live message handle that is not concurrently
/// mutated or freed. NULL and panic both produce zero.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn datarouter_msg_flags(msg: *const datarouter_message) -> u8 {
    ffi_or(0, || {
        if msg.is_null() {
            0
        } else {
            unsafe { (*msg).inner.flags }
        }
    })
}

/// Set the message flags.
///
/// # Safety
/// `msg` must be NULL or a live, exclusively accessible message handle. No
/// other thread may read, mutate, or free it during this call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn datarouter_msg_set_flags(msg: *mut datarouter_message, flags: u8) {
    ffi_or((), || {
        if !msg.is_null() {
            unsafe { (*msg).inner.flags = flags };
        }
    });
}

/// Get the var_data pointer. Returns NULL if empty.
///
/// The returned pointer is borrowed and remains valid only until the message
/// is mutated or freed.
///
/// # Safety
/// `msg` must be NULL or a live message handle that is not concurrently
/// mutated or freed. NULL and panic both produce NULL.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn datarouter_msg_var_data(msg: *const datarouter_message) -> *const u8 {
    ffi_or(ptr::null(), || {
        if msg.is_null() {
            return ptr::null();
        }
        let msg = unsafe { &(*msg).inner };
        if msg.var_data.is_empty() {
            ptr::null()
        } else {
            msg.var_data.as_ptr()
        }
    })
}

/// Get the var_data length.
///
/// # Safety
/// `msg` must be NULL or a live message handle that is not concurrently
/// mutated or freed. NULL and panic both produce zero.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn datarouter_msg_var_data_len(msg: *const datarouter_message) -> usize {
    ffi_or(0, || {
        if msg.is_null() {
            0
        } else {
            unsafe { (*msg).inner.var_data.len() }
        }
    })
}

/// Get the body pointer. Returns NULL if empty.
///
/// The returned pointer is borrowed and remains valid only until the message
/// is mutated or freed.
///
/// # Safety
/// `msg` must be NULL or a live message handle that is not concurrently
/// mutated or freed. NULL and panic both produce NULL.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn datarouter_msg_body(msg: *const datarouter_message) -> *const u8 {
    ffi_or(ptr::null(), || {
        if msg.is_null() {
            return ptr::null();
        }
        let msg = unsafe { &(*msg).inner };
        if msg.body.is_empty() {
            ptr::null()
        } else {
            msg.body.as_ptr()
        }
    })
}

/// Get the body length.
///
/// # Safety
/// `msg` must be NULL or a live message handle that is not concurrently
/// mutated or freed. NULL and panic both produce zero.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn datarouter_msg_body_len(msg: *const datarouter_message) -> usize {
    ffi_or(0, || {
        if msg.is_null() {
            0
        } else {
            unsafe { (*msg).inner.body.len() }
        }
    })
}

// ── Constant Getters ──────────────────────────────────────────────────────

#[unsafe(no_mangle)]
pub extern "C" fn datarouter_tag_module() -> u8 {
    protocol::TAG_MODULE
}
#[unsafe(no_mangle)]
pub extern "C" fn datarouter_tag_action() -> u8 {
    protocol::TAG_ACTION
}
#[unsafe(no_mangle)]
pub extern "C" fn datarouter_tag_streaming() -> u8 {
    protocol::TAG_STREAMING
}
#[unsafe(no_mangle)]
pub extern "C" fn datarouter_tag_request_id() -> u8 {
    protocol::TAG_REQUEST_ID
}
#[unsafe(no_mangle)]
pub extern "C" fn datarouter_tag_connection_id() -> u8 {
    protocol::TAG_CONNECTION_ID
}
#[unsafe(no_mangle)]
pub extern "C" fn datarouter_tag_context() -> u8 {
    protocol::TAG_CONTEXT
}
#[unsafe(no_mangle)]
pub extern "C" fn datarouter_tag_chunk_id() -> u8 {
    protocol::TAG_CHUNK_ID
}
#[unsafe(no_mangle)]
pub extern "C" fn datarouter_tag_chunk_index() -> u8 {
    protocol::TAG_CHUNK_INDEX
}
#[unsafe(no_mangle)]
pub extern "C" fn datarouter_tag_chunk_count() -> u8 {
    protocol::TAG_CHUNK_COUNT
}
#[unsafe(no_mangle)]
pub extern "C" fn datarouter_tag_status() -> u8 {
    protocol::TAG_STATUS
}
#[unsafe(no_mangle)]
pub extern "C" fn datarouter_tag_is_text() -> u8 {
    protocol::TAG_IS_TEXT
}
#[unsafe(no_mangle)]
pub extern "C" fn datarouter_tag_json_data() -> u8 {
    protocol::TAG_JSON_DATA
}
#[unsafe(no_mangle)]
pub extern "C" fn datarouter_tag_workflow_id() -> u8 {
    protocol::TAG_WORKFLOW_ID
}
#[unsafe(no_mangle)]
pub extern "C" fn datarouter_tag_task_uuid() -> u8 {
    protocol::TAG_TASK_UUID
}
#[unsafe(no_mangle)]
pub extern "C" fn datarouter_tag_step_id() -> u8 {
    protocol::TAG_STEP_ID
}
#[unsafe(no_mangle)]
pub extern "C" fn datarouter_tag_actor_uuid() -> u8 {
    protocol::TAG_ACTOR_UUID
}
#[unsafe(no_mangle)]
pub extern "C" fn datarouter_tag_trace_id() -> u8 {
    protocol::TAG_TRACE_ID
}

#[unsafe(no_mangle)]
pub extern "C" fn datarouter_msg_business_request() -> u8 {
    protocol::MSG_BUSINESS_REQUEST
}
#[unsafe(no_mangle)]
pub extern "C" fn datarouter_msg_business_response() -> u8 {
    protocol::MSG_BUSINESS_RESPONSE
}
#[unsafe(no_mangle)]
pub extern "C" fn datarouter_msg_chunk_ack() -> u8 {
    protocol::MSG_CHUNK_ACK
}
#[unsafe(no_mangle)]
pub extern "C" fn datarouter_msg_chunk_error() -> u8 {
    protocol::MSG_CHUNK_ERROR
}
#[unsafe(no_mangle)]
pub extern "C" fn datarouter_msg_routes_refresh() -> u8 {
    protocol::MSG_ROUTES_REFRESH
}
#[unsafe(no_mangle)]
pub extern "C" fn datarouter_msg_cache_invalidate() -> u8 {
    protocol::MSG_CACHE_INVALIDATE
}
#[unsafe(no_mangle)]
pub extern "C" fn datarouter_msg_agent_registry_update() -> u8 {
    protocol::MSG_AGENT_REGISTRY_UPDATE
}
#[unsafe(no_mangle)]
pub extern "C" fn datarouter_msg_agent_request() -> u8 {
    protocol::MSG_AGENT_REQUEST
}
#[unsafe(no_mangle)]
pub extern "C" fn datarouter_msg_agent_event() -> u8 {
    protocol::MSG_AGENT_EVENT
}
#[unsafe(no_mangle)]
pub extern "C" fn datarouter_msg_control() -> u8 {
    protocol::MSG_CONTROL
}
#[unsafe(no_mangle)]
pub extern "C" fn datarouter_msg_log_notice() -> u8 {
    protocol::MSG_LOG_NOTICE
}
#[unsafe(no_mangle)]
pub extern "C" fn datarouter_max_app_payload() -> usize {
    protocol::MAX_APP_PAYLOAD
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn panic_containment_uses_abi_category_defaults() {
        let status = ffi_or(crate::DATAROUTER_ERR_INTERNAL, || -> c_int {
            panic!("status panic")
        });
        assert_eq!(status, crate::DATAROUTER_ERR_INTERNAL);

        let constructor = ffi_or(ptr::null_mut(), || -> *mut datarouter_message {
            panic!("constructor panic")
        });
        assert!(constructor.is_null());

        let accessor = ffi_or(0_u8, || -> u8 { panic!("accessor panic") });
        assert_eq!(accessor, 0);

        ffi_or((), || -> () { panic!("void panic") });
    }

    #[test]
    fn pack_reports_required_capacity_without_consuming_output() {
        let msg = unsafe { datarouter_msg_new(protocol::MSG_CONTROL) };
        assert!(!msg.is_null());

        let mut required = usize::MAX;
        let code = unsafe { datarouter_msg_pack(msg, ptr::null_mut(), 0, &mut required) };
        assert_eq!(code, crate::DATAROUTER_ERR_PAYLOAD_TOO_LARGE);
        assert_eq!(required, protocol::HEADER_SIZE);

        let mut output = vec![0_u8; required];
        let code = unsafe {
            datarouter_msg_pack(msg, output.as_mut_ptr().cast(), output.len(), &mut required)
        };
        assert_eq!(code, crate::DATAROUTER_OK);
        assert_eq!(required, protocol::HEADER_SIZE);

        unsafe { datarouter_msg_free(msg) };
    }

    #[test]
    fn unpack_clears_output_before_rejecting_input() {
        let mut out = ptr::dangling_mut::<datarouter_message>();
        let code = unsafe { datarouter_msg_unpack(ptr::null(), 0, &mut out) };
        assert_eq!(code, crate::DATAROUTER_ERR_PROTOCOL);
        assert!(out.is_null());

        out = ptr::dangling_mut();
        let code = unsafe { datarouter_msg_unpack(ptr::null(), 1, &mut out) };
        assert_eq!(code, crate::DATAROUTER_ERR_INVALID_ARGUMENT);
        assert!(out.is_null());
    }

    #[test]
    fn oversized_ffi_tlv_is_marked_unencodable_instead_of_truncated() {
        let msg = unsafe { datarouter_msg_new(protocol::MSG_CONTROL) };
        assert!(!msg.is_null());
        let value = vec![0xA5_u8; u16::MAX as usize + 1];
        unsafe {
            datarouter_tlv_put_bytes(msg, 0x42, value.as_ptr().cast(), value.len());
            datarouter_tlv_put_byte(msg, 0x43, 0);
        }

        let mut required = usize::MAX;
        let code = unsafe { datarouter_msg_pack(msg, ptr::null_mut(), 0, &mut required) };
        assert_eq!(code, crate::DATAROUTER_ERR_PROTOCOL);
        assert_eq!(required, 0);

        unsafe { datarouter_msg_free(msg) };
    }

    #[test]
    fn output_pointer_length_pairs_follow_the_common_rule() {
        let msg = unsafe { datarouter_msg_new(protocol::MSG_CONTROL) };
        assert!(!msg.is_null());
        let value = [1_u8, 2, 3];
        unsafe {
            datarouter_tlv_put_bytes(msg, 0x42, value.as_ptr().cast(), value.len());
        }

        let mut actual = usize::MAX;
        let code = unsafe { datarouter_tlv_read_bytes(msg, 0x42, ptr::null_mut(), 0, &mut actual) };
        assert_eq!(code, crate::DATAROUTER_OK);
        assert_eq!(actual, value.len());

        actual = usize::MAX;
        let code = unsafe { datarouter_tlv_read_bytes(msg, 0x42, ptr::null_mut(), 1, &mut actual) };
        assert_eq!(code, crate::DATAROUTER_ERR_INVALID_ARGUMENT);
        assert_eq!(actual, 0);

        unsafe { datarouter_msg_free(msg) };
    }
}
