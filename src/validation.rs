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
    fn datarouter_protocol_chunk_metadata(
        data: *const u8,
        len: usize,
        out: *mut datarouter_chunk_meta,
    ) -> c_int;
    fn datarouter_protocol_strip_chunk(
        data: *const u8,
        len: usize,
        out: *mut u8,
        cap: usize,
        out_len: *mut usize,
    ) -> c_int;
    fn datarouter_protocol_ack_fields(
        data: *const u8,
        len: usize,
        id: *mut u64,
        index: *mut u16,
    ) -> c_int;
    fn datarouter_protocol_classify_frame(
        data: *const u8,
        buf_len: usize,
        layout: *mut datarouter_frame_layout,
        chunk: *mut datarouter_chunk_meta,
    ) -> c_int;
    fn datarouter_tlv_append(
        buf: *mut u8,
        cap: usize,
        len: usize,
        tag: u8,
        data: *const u8,
        data_len: usize,
        out_new_len: *mut usize,
    ) -> c_int;
    fn datarouter_tlv_find(
        data: *const u8,
        data_len: usize,
        tag: u8,
        start: usize,
        out_value: *mut *const u8,
        out_len: *mut usize,
    ) -> c_int;
    fn datarouter_tlv_next(
        data: *const u8,
        data_len: usize,
        cursor: *mut usize,
        out_tag: *mut u8,
        out_value: *mut *const u8,
        out_len: *mut usize,
    ) -> c_int;
}

/// Must match `datarouter_frame_layout` in `include/datarouter_protocol.h`
/// and `src/validation.c` (plain repr(C), no padding beyond natural).
#[repr(C)]
#[derive(Clone, Copy)]
struct datarouter_frame_layout {
    message_type: u8,
    flags: u8,
    var_data_length: u16,
    body_length: u32,
    total_length: u32,
}

/// Must match `datarouter_chunk_meta` in `include/datarouter_protocol.h`.
#[repr(C)]
#[derive(Clone, Copy)]
struct datarouter_chunk_meta {
    present: c_int,
    has_chunk: u8,
    chunk_id: u64,
    chunk_index: u32,
    chunk_count: u32,
    common_len: usize,
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

/// Chunk-classification outcome of a single-pass C TLV scan.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ChunkClass {
    /// Valid TLV, no chunk metadata at all.
    None,
    /// Valid TLV with all three chunk metadata fields present.
    Complete { id: u64, index: u16, count: u16, common_len: usize },
    /// Valid TLV with some but not all chunk metadata fields.
    Incomplete,
    /// Malformed TLV or invalid/duplicate chunk field.
    Invalid,
}

/// Single-pass validate + classify via C. Subsumes the previous
/// `validate_tlvs`-then-re-walk used by `raw_is_single_frame` and
/// `parse_chunk_metadata`.
#[inline]
pub(crate) fn chunk_classify(var_data: &[u8]) -> ChunkClass {
    let ptr = if var_data.is_empty() {
        std::ptr::null()
    } else {
        var_data.as_ptr()
    };
    let mut out = std::mem::MaybeUninit::<datarouter_chunk_meta>::uninit();
    // SAFETY: ptr readable for len bytes (or NULL/0); out is a writable
    // aligned MaybeUninit. C always fully initializes *out when it returns
    // DATAROUTER_OK; it returns INVALID_ARGUMENT only for NULL out (never here).
    let code = unsafe { datarouter_protocol_chunk_metadata(ptr, var_data.len(), out.as_mut_ptr()) };
    debug_assert_eq!(code, crate::DATAROUTER_OK);
    // SAFETY: C fully initializes *out on DATAROUTER_OK (always reached).
    let out = unsafe { out.assume_init() };
    match out.present {
        0 => ChunkClass::None,
        1 => ChunkClass::Complete {
            id: out.chunk_id,
            index: out.chunk_index as u16,
            count: out.chunk_count as u16,
            common_len: out.common_len,
        },
        -1 => ChunkClass::Incomplete,
        _ => ChunkClass::Invalid,
    }
}

/// Copy the non-chunk TLV bytes via C (present==1 input); otherwise returns
/// the original bytes, mirroring `protocol::strip_chunk_metadata`.
pub(crate) fn strip_chunk(var_data: &[u8]) -> Vec<u8> {
    let mut required = 0usize;
    let ptr = if var_data.is_empty() {
        std::ptr::null()
    } else {
        var_data.as_ptr()
    };
    // SAFETY: ptr readable for len bytes (or NULL/0); out_len is a valid
    // writable pointer. With cap=0 and out=NULL we only query the length.
    let code = unsafe {
        datarouter_protocol_strip_chunk(ptr, var_data.len(), std::ptr::null_mut(), 0, &mut required)
    };
    debug_assert_eq!(code, crate::DATAROUTER_OK);
    let mut out = vec![0u8; required];
    if required != 0 {
        // SAFETY: out is writable for exactly `required` bytes, which the C
        // function reported. Since we trust its length report and it only
        // writes within [0, required], this cannot overflow.
        let code = unsafe {
            datarouter_protocol_strip_chunk(
                ptr,
                var_data.len(),
                out.as_mut_ptr(),
                out.len(),
                &mut required,
            )
        };
        debug_assert_eq!(code, crate::DATAROUTER_OK);
        out.truncate(required);
    }
    out
}

/// Result of a single-pass C frame classification (header + TLV classify).
pub(crate) struct ClassifiedFrame {
    pub message_type: u8,
    pub flags: u8,
    pub var_data_len: usize,
    pub body_len: usize,
    pub chunk: ChunkClass,
}

/// Single-pass frame classification via C: exact frame-header/length check and
/// var_data chunk-metadata classify in one FFI crossing (copy-neutral).
#[inline]
pub(crate) fn classify_frame(raw: &[u8]) -> Option<ClassifiedFrame> {
    let ptr = if raw.is_empty() {
        std::ptr::null()
    } else {
        raw.as_ptr()
    };
    let mut layout = std::mem::MaybeUninit::<datarouter_frame_layout>::uninit();
    let mut chunk = std::mem::MaybeUninit::<datarouter_chunk_meta>::uninit();
    // SAFETY: ptr readable for len bytes (or NULL/0); layout/chunk are writable
    // aligned MaybeUninit. C leaves them uninitialized on error, so we only read
    // them after DATAROUTER_OK.
    let code = unsafe {
        datarouter_protocol_classify_frame(ptr, raw.len(), layout.as_mut_ptr(), chunk.as_mut_ptr())
    };
    if code != crate::DATAROUTER_OK {
        return None;
    }
    // SAFETY: C fully initializes both on DATAROUTER_OK.
    let layout = unsafe { layout.assume_init() };
    let out = unsafe { chunk.assume_init() };
    let chunk = match out.present {
        0 => ChunkClass::None,
        1 => ChunkClass::Complete {
            id: out.chunk_id,
            index: out.chunk_index as u16,
            count: out.chunk_count as u16,
            common_len: out.common_len,
        },
        -1 => ChunkClass::Incomplete,
        _ => ChunkClass::Invalid,
    };
    Some(ClassifiedFrame {
        message_type: layout.message_type,
        flags: layout.flags,
        var_data_len: layout.var_data_length as usize,
        body_len: layout.body_length as usize,
        chunk,
    })
}

/// Extract the (chunk_id, chunk_index) pair from a chunk-control message's
/// var_data via C (`protocol::chunk_control_fields_from_var_data`).
#[inline]
pub(crate) fn ack_fields(var_data: &[u8]) -> Option<(u64, u16)> {
    let ptr = if var_data.is_empty() {
        std::ptr::null()
    } else {
        var_data.as_ptr()
    };
    let mut id = 0u64;
    let mut index = 0u16;
    // SAFETY: ptr readable for len bytes (or NULL/0); id/index point to
    // writable, aligned locals. C never writes them unless it returns OK.
    let code = unsafe { datarouter_protocol_ack_fields(ptr, var_data.len(), &mut id, &mut index) };
    if code == crate::DATAROUTER_OK {
        Some((id, index))
    } else {
        None
    }
}

/// Append one TLV into a Rust `Vec<u8>`, growing as needed, via the C
/// single source of truth. This is the ONLY copy on the builder path (the C
/// memcpy of value -> block). `data` must not alias `buf`.
///
/// Returns `Err(&'static str)` with the same messages the historical Rust
/// builder used (`"TLV value exceeds u16"`, `"TLV builder length overflow"`),
/// so callers' error strings are unchanged.
pub(crate) fn tlv_append(buf: &mut Vec<u8>, tag: u8, data: &[u8]) -> Result<(), &'static str> {
    if data.len() > u16::MAX as usize {
        return Err("TLV value exceeds u16");
    }
    let Some(need) = 3usize.checked_add(data.len()) else {
        return Err("TLV builder length overflow");
    };
    if buf.len().checked_add(need).is_none() {
        return Err("TLV builder length overflow");
    }
    let mine = (buf.capacity() - buf.len()) < need;
    if mine {
        buf.reserve(need);
    }
    if (buf.capacity() - buf.len()) < need {
        // Safety net: C would reject with FULL; treat as builder length overflow.
        return Err("TLV builder length overflow");
    }
    let mut new_len = 0usize;
    // SAFETY: buf is a live Vec with capacity >= len+need (reserved above), so
    // C writes exactly need bytes at offset len within capacity. data is a
    // disjoint readable region owned by the caller. set_len below adopts the
    // C-written bytes.
    let code = unsafe {
        datarouter_tlv_append(
            buf.as_mut_ptr(),
            buf.capacity(),
            buf.len(),
            tag,
            if data.is_empty() {
                std::ptr::null()
            } else {
                data.as_ptr()
            },
            data.len(),
            &mut new_len,
        )
    };
    match code {
        crate::DATAROUTER_OK => {
            // SAFETY: new_len <= capacity (C only writes within cap) and >= len.
            unsafe { buf.set_len(new_len) };
            Ok(())
        }
        crate::DATAROUTER_ERR_PAYLOAD_TOO_LARGE => Err("TLV value exceeds u16"),
        _ => Err("TLV builder length overflow"),
    }
}

/// Single-pass validate-and-find over a full TLV block via C. Returns
/// `Ok(Some((value_off, value_len)))` on a match (offsets borrow var_data),
/// `Ok(None)` on a clean block without the tag, `Err(())` on a malformed block.
pub(crate) fn tlv_find(
    var_data: &[u8],
    tag: u8,
    start: usize,
) -> Result<Option<(usize, usize)>, ()> {
    let data = if var_data.is_empty() {
        std::ptr::null()
    } else {
        var_data.as_ptr()
    };
    let mut value: *const u8 = std::ptr::null();
    let mut out_len = 0usize;
    // SAFETY: data readable for var_data.len() bytes (or NULL/0). C sets
    // value/out_len only on DATAROUTER_OK and never writes out of bounds.
    let code = unsafe {
        datarouter_tlv_find(data, var_data.len(), tag, start, &mut value, &mut out_len)
    };
    match code {
        crate::DATAROUTER_OK => {
            let off = (value as usize).wrapping_sub(var_data.as_ptr() as usize);
            if off + out_len > var_data.len() {
                return Err(());
            }
            Ok(Some((off, out_len)))
        }
        crate::DATAROUTER_ERR_NOT_FOUND => Ok(None),
        _ => Err(()),
    }
}

/// In-order iterate over a TLV block via C. Returns the (tag, value_off,
/// value_len) entries, or `Err` with `"truncated TLV header"` /
/// `"truncated TLV value"` on malformed input (distinct C outcome codes).
pub(crate) fn tlv_walk(var_data: &[u8]) -> Result<Vec<(u8, usize, usize)>, &'static str> {
    let data = if var_data.is_empty() {
        std::ptr::null()
    } else {
        var_data.as_ptr()
    };
    let len = var_data.len();
    let mut cursor = 0usize;
    let mut out = Vec::new();
    loop {
        let mut tag = 0u8;
        let mut value: *const u8 = std::ptr::null();
        let mut value_len = 0usize;
        // SAFETY: data readable for len bytes; C only advances cursor within
        // len and writes out* on DECODED.
        let code = unsafe {
            datarouter_tlv_next(
                data,
                len,
                &mut cursor,
                &mut tag,
                &mut value,
                &mut value_len,
            )
        };
        if code == 1 {
            let off = (value as usize).wrapping_sub(var_data.as_ptr() as usize);
            if off + value_len > len {
                return Err("truncated TLV value");
            }
            out.push((tag, off, value_len));
        } else if code == 0 {
            break;
        } else if code == -1 {
            return Err("truncated TLV header");
        } else {
            return Err("truncated TLV value");
        }
    }
    Ok(out)
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
    fn c_chunk_classify_outcomes() {
        use crate::validation::ChunkClass;
        // No chunk tags: module/action TLVs only.
        let plain = [0x0F, 2, 0, b'm', b'p'];
        assert_eq!(chunk_classify(&plain), ChunkClass::None);

        // Complete chunk metadata: id(8B) + index(2B) + count(2B).
        let mut vd = crate::protocol::VarDataBuilder::new();
        vd.chunk_id(0x1122334455667788)
            .chunk_index(3)
            .chunk_count(5);
        let bytes = vd.build();
        assert_eq!(
            chunk_classify(&bytes),
            ChunkClass::Complete {
                id: 0x1122334455667788,
                index: 3,
                count: 5,
                common_len: 0,
            }
        );

        // Incomplete: id only.
        let mut id_only = crate::protocol::VarDataBuilder::new();
        id_only.chunk_id(42);
        assert_eq!(chunk_classify(&id_only.build()), ChunkClass::Incomplete);

        // Invalid: duplicate index.
        let dup = [0x09, 2, 0, 0x03, 0x00, 0x09, 2, 0, 0x04, 0x00];
        assert_eq!(chunk_classify(&dup), ChunkClass::Invalid);
        // Invalid: malformed TLV.
        assert_eq!(chunk_classify(&[0x09, 2, 0, 0x03]), ChunkClass::Invalid);
    }

    #[test]
    fn c_strip_chunk_keeps_non_chunk_tlvs() {
        let mut vd = crate::protocol::VarDataBuilder::new();
        vd.module("m")
            .action("a")
            .chunk_id(99)
            .chunk_index(0)
            .chunk_count(2);
        let bytes = vd.build();

        let stripped = strip_chunk(&bytes);
        // only module+action TLVs remain (6 + 6 bytes), chunk TLVs removed.
        assert_eq!(stripped, [0x0F, 1, 0, b'm', 0x10, 1, 0, b'a']);

        // Non-chunk input returns original bytes unchanged.
        let plain = [0x0F, 1, 0, b'm'];
        assert_eq!(strip_chunk(&plain), plain);
    }

    #[test]
    fn c_ack_fields_extracts_pair() {
        let mut vd = crate::protocol::VarDataBuilder::new();
        vd.chunk_id(0xABCD).chunk_index(7);
        assert_eq!(ack_fields(&vd.build()), Some((0xABCD, 7)));

        // missing index -> None
        let mut id_only = crate::protocol::VarDataBuilder::new();
        id_only.chunk_id(1);
        assert_eq!(ack_fields(&id_only.build()), None);

        // duplicate id -> None
        let dup = [
            0x08, 8, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0x08, 8, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0,
        ];
        assert_eq!(ack_fields(&dup), None);

        // non-chunk -> None
        assert_eq!(ack_fields(&[0x0F, 1, 0, b'm']), None);
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

    #[test]
    fn c_tlv_append_find_next_round_trip() {
        let mut block: Vec<u8> = Vec::new();
        assert!(tlv_append(&mut block, 0x0F, b"admin").is_ok());
        assert!(tlv_append(&mut block, 0x10, b"test").is_ok());
        let expect = [
            0x0F, 5, 0, b'a', b'd', b'm', b'i', b'n', // module="admin"
            0x10, 4, 0, b't', b'e', b's', b't', // action="test"
        ];
        assert_eq!(block, expect);

        // find returns value offset + length (borrowing block) .
        let (off, len) = tlv_find(&block, 0x10, 0).unwrap().unwrap();
        assert_eq!(&block[off..off + len], b"test");
        assert_eq!(tlv_find(&block, 0x99, 0).unwrap(), None);

        // in-order walk.
        let walk = tlv_walk(&block).unwrap();
        assert_eq!(walk.len(), 2);
        assert_eq!(&block[walk[0].1..walk[0].1 + walk[0].2], b"admin");
        assert_eq!(&block[walk[1].1..walk[1].1 + walk[1].2], b"test");
    }

    #[test]
    fn c_tlv_truncation_codes_are_distinct() {
        // truncated value: header says 4 but only 1 byte follows.
        assert_eq!(tlv_walk(&[0x10, 4, 0, b'x']).unwrap_err(), "truncated TLV value");
        // truncated header: 2 bytes remain, not enough for a 3-byte header.
        assert_eq!(tlv_walk(&[0x10, 4]).unwrap_err(), "truncated TLV header");
    }

    #[test]
    fn c_tlv_append_rejects_oversized_and_grows() {
        // oversized value (> u16) is rejected with the builder error string.
        let big = vec![0u8; 0x1_0000];
        let mut block: Vec<u8> = Vec::new();
        assert_eq!(
            tlv_append(&mut block, 0x0F, &big).unwrap_err(),
            "TLV value exceeds u16"
        );
        // growth across many appends keeps everything intact.
        let mut block: Vec<u8> = Vec::new();
        for i in 0u8..40 {
            tlv_append(&mut block, i, &[i; 7]).unwrap();
        }
        assert_eq!(block.len(), 40 * 10);
        assert_eq!(tlv_find(&block, 39, 0).unwrap().unwrap(), (39 * 10 + 3, 7usize));
    }
}
