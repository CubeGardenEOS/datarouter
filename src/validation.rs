//! Rust delegation to the C protocol-validation single source of truth.
//!
//! The canonical validation rules live in `src/validation.c`. Rust MUST NOT
//! carry a second, independently-maintained implementation of the same rules;
//! it only wraps the C functions via FFI. This keeps the Rust/C/Go/Python
//! bindings from drifting apart and satisfies the project constraint that
//! validation lives in the C layer.

use std::os::raw::c_int;

unsafe extern "C" {
    fn datarouter_protocol_validate_tlvs(data: *const u8, len: usize) -> c_int;
    fn datarouter_protocol_frame_layout(
        data: *const u8,
        buf_len: usize,
        out: *mut datarouter_frame_layout,
    ) -> c_int;
}

/// Must match `datarouter_frame_layout` in `include/datarouter_protocol.h`
/// and `src/validation.c` (plain repr(C), no padding beyond natural).
#[repr(C)]
struct datarouter_frame_layout {
    message_type: u8,
    flags: u8,
    var_data_length: u16,
    body_length: u32,
    total_length: u32,
}

/// Validate a TLV block via the C single source of truth.
#[inline]
pub(crate) fn validate_tlvs(data: &[u8]) -> bool {
    let ptr = if data.is_empty() {
        std::ptr::null()
    } else {
        data.as_ptr()
    };
    // SAFETY: `ptr` is non-NULL exactly when `len > 0` and points to exactly
    // `len` readable bytes (from a &[u8]). An empty slice passes NULL/0 which
    // is valid and never dereferenced. The C function only reads within bounds.
    unsafe { datarouter_protocol_validate_tlvs(ptr, data.len()) != 0 }
}

/// Parse and validate a frame header against its full buffer length via the C
/// single source of truth. Returns `(msg_type, flags, var_data_len, body_len)`
/// on an exact match (buf_len == 8 + var_data + body), else `None`.
#[inline]
pub(crate) fn frame_layout(data: &[u8]) -> Option<(u8, u8, usize, usize)> {
    let ptr = if data.is_empty() {
        std::ptr::null()
    } else {
        data.as_ptr()
    };
    let mut out = std::mem::MaybeUninit::<datarouter_frame_layout>::uninit();
    // SAFETY: `ptr` is readable for `len` bytes (or NULL/0); `out` is a
    // writable, properly aligned MaybeUninit. The C function returns with
    // *out uninitialized on error, so we only read it on DATAROUTER_OK.
    let code = unsafe { datarouter_protocol_frame_layout(ptr, data.len(), out.as_mut_ptr()) };
    if code == crate::DATAROUTER_OK {
        // SAFETY: on DATAROUTER_OK the C function fully initialized *out.
        let out = unsafe { out.assume_init() };
        Some((
            out.message_type,
            out.flags,
            out.var_data_length as usize,
            out.body_length as usize,
        ))
    } else {
        None
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    // Mirror the semantics tests for `protocol::validate_tlvs` at the FFI
    // boundary: the C implementation is the single source of truth.
    #[test]
    fn c_validation_matches_expected_semantics() {
        // empty block is valid
        assert!(validate_tlvs(b""));
        // complete single entry: tag=0x0F, len=5, "admin"
        assert!(validate_tlvs(&[0x0F, 5, 0, b'a', b'd', b'm', b'i', b'n']));
        // truncated value
        assert!(!validate_tlvs(&[0x0F, 5, 0, b'a', b'd']));
        // truncated header (fewer than 3 bytes remain)
        assert!(!validate_tlvs(&[0x0F, 4]));
        // two complete entries back to back
        let two = [0x0F, 2, 0, b'm', b'p', 0x10, 1, 0, b'x'];
        assert!(validate_tlvs(&two));
        // trailing 1 byte after a complete entry => truncated header
        assert!(!validate_tlvs(&[0x0F, 1, 0, b'a', 0x42]));
    }

    #[test]
    fn c_frame_layout_exact_match() {
        // header: type=0x07 (req), flags=0, vd_len=6, body_len=5
        let frame = [
            0x07, 0x00, 0x06, 0x00, 0x05, 0x00, 0x00, 0x00, // 8-byte header
            0x0F, 3, 0, b'a', b'd', b'm', // var_data (6 bytes)
            b'h', b'e', b'l', b'l', b'o', // body (5 bytes)
        ];
        assert_eq!(frame.len(), 8 + 6 + 5);
        assert_eq!(
            frame_layout(&frame),
            Some((0x07, 0x00, 6, 5))
        );
        // truncated body
        assert_eq!(frame_layout(&frame[..frame.len() - 1]), None);
        // trailing byte
        let mut extra = frame.to_vec();
        extra.push(0x00);
        assert_eq!(frame_layout(&extra), None);
        // too short for a header
        assert_eq!(frame_layout(&frame[..6]), None);
        // empty
        assert_eq!(frame_layout(b""), None);
    }
}
