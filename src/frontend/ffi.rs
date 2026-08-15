use std::os::raw::{c_char, c_int, c_void};
use std::panic::{self, AssertUnwindSafe};
use std::ptr;
use std::slice;

use super::options::{FrontendOptions, read_c_string};
use super::{Frontend, RecvPackedError};

pub const DATAROUTER_FRONTEND_ABI_VERSION: u32 = 2;

/// ABI version of the v3 options struct ([`datarouter_frontend_options_v3`]).
pub const DATAROUTER_FRONTEND_ABI_VERSION_V3: u32 = 3;

/// Direct (unsafe) data plane: 2 copies, single-frame messages.
pub const DATAROUTER_FRONTEND_DATA_PLANE_DIRECT: u8 = 0;
/// Routed (safe) data plane: switch-routed, queue + chunking/ACK/reassembly.
pub const DATAROUTER_FRONTEND_DATA_PLANE_ROUTED: u8 = 1;

#[repr(C)]
#[derive(Debug, Copy, Clone)]
pub struct datarouter_frontend_options {
    pub shm_name: *const c_char,
    pub service_name: *const c_char,
    pub connect_timeout_ms: u32,
    pub recv_timeout_ms: u32,
    pub ack_timeout_ms: u32,
    pub reader_wait_ms: u32,
    pub app_queue_capacity: usize,
    pub max_message_bytes: usize,
    pub max_pending_acks: usize,
    pub chunk_ttl_ms: u32,
    pub max_reassembly_groups: usize,
    pub max_reassembly_bytes: usize,
}

#[repr(C)]
#[derive(Debug, Copy, Clone)]
pub struct datarouter_frontend_options_v3 {
    pub shm_name: *const c_char,
    pub service_name: *const c_char,
    pub connect_timeout_ms: u32,
    pub recv_timeout_ms: u32,
    pub ack_timeout_ms: u32,
    pub reader_wait_ms: u32,
    pub app_queue_capacity: usize,
    pub max_message_bytes: usize,
    pub max_pending_acks: usize,
    pub chunk_ttl_ms: u32,
    pub max_reassembly_groups: usize,
    pub max_reassembly_bytes: usize,
    /// Optional shared secret; required when the switch runs with `--token`.
    pub auth_token: *const c_char,
    /// [`DATAROUTER_FRONTEND_DATA_PLANE_DIRECT`] (default) or
    /// [`DATAROUTER_FRONTEND_DATA_PLANE_ROUTED`].
    pub data_plane: u8,
}

#[repr(C)]
#[derive(Debug, Copy, Clone, Default)]
pub struct datarouter_frontend_stats {
    pub sent_frames: u64,
    pub sent_chunks: u64,
    pub received_frames: u64,
    pub received_complete: u64,
    pub received_chunk_acks: u64,
    pub received_chunk_errors: u64,
    pub ack_timeouts: u64,
    pub queue_full: u64,
    pub protocol_errors: u64,
    pub dropped_frames: u64,
    pub pending_acks: u64,
    pub queued_messages: u64,
    pub reader_alive: u64,
    pub reader_errors: u64,
    pub keepalive_ticks: u64,
    pub last_keepalive_ns: u64,
    pub reassembly_expired: u64,
    pub reassembly_rejected: u64,
    pub corrupted_rings: u64,
    pub registration_recoveries: u64,
    pub blocked_wakeups: u64,
    pub dead_process_reaps: u64,
    pub transport_generation: u64,
    pub switch_recoveries: u64,
}

#[repr(C)]
pub struct datarouter_frontend {
    frontend: Frontend,
}

fn ffi_status(operation: impl FnOnce() -> c_int) -> c_int {
    panic::catch_unwind(AssertUnwindSafe(operation)).unwrap_or(crate::DATAROUTER_ERR_INTERNAL)
}

#[unsafe(no_mangle)]
/// Open a frontend and return an owned opaque handle.
///
/// # Safety
/// `out` must be writable. This legacy entry point does not dereference
/// `options`; it clears `*out` and reports an ABI mismatch.
pub unsafe extern "C" fn datarouter_frontend_open(
    _options: *const datarouter_frontend_options,
    out: *mut *mut datarouter_frontend,
) -> c_int {
    ffi_status(|| {
        if out.is_null() {
            return crate::DATAROUTER_ERR_INVALID_ARGUMENT;
        }
        unsafe { *out = ptr::null_mut() };
        crate::DATAROUTER_ERR_PROTOCOL
    })
}

#[unsafe(no_mangle)]
pub extern "C" fn datarouter_frontend_abi_version() -> u32 {
    DATAROUTER_FRONTEND_ABI_VERSION
}

#[unsafe(no_mangle)]
/// Open a frontend using the size-checked frontend ABI.
///
/// # Safety
/// `out` must be writable. When the size and ABI version match, `options` must
/// point to a complete readable `datarouter_frontend_options`, and its strings
/// must be NUL-terminated for this call. `*out` is cleared on every failure.
pub unsafe extern "C" fn datarouter_frontend_open_v2(
    options: *const datarouter_frontend_options,
    options_size: usize,
    abi_version: u32,
    out: *mut *mut datarouter_frontend,
) -> c_int {
    ffi_status(|| {
        if out.is_null() {
            return crate::DATAROUTER_ERR_INVALID_ARGUMENT;
        }
        unsafe { *out = ptr::null_mut() };
        if options.is_null()
            || options_size != std::mem::size_of::<datarouter_frontend_options>()
            || abi_version != DATAROUTER_FRONTEND_ABI_VERSION
        {
            return crate::DATAROUTER_ERR_PROTOCOL;
        }
        let options = match unsafe { frontend_options_from_c(options) } {
            Ok(options) => options,
            Err(code) => return code,
        };
        match Frontend::connect(options) {
            Ok(frontend) => {
                let handle = Box::new(datarouter_frontend { frontend });
                unsafe { *out = Box::into_raw(handle) };
                crate::DATAROUTER_OK
            }
            Err(code) => code,
        }
    })
}

#[unsafe(no_mangle)]
/// Open a frontend with the v3 options struct: adds `auth_token` and an
/// explicit `data_plane` selection (direct is the default, matching v2's
/// semantics).
///
/// # Safety
/// `out` must be writable. When the size and ABI version match, `options` must
/// point to a complete readable `datarouter_frontend_options_v3`, and its
/// strings must be NUL-terminated for this call. `*out` is cleared on every
/// failure.
pub unsafe extern "C" fn datarouter_frontend_open_v3(
    options: *const datarouter_frontend_options_v3,
    options_size: usize,
    abi_version: u32,
    out: *mut *mut datarouter_frontend,
) -> c_int {
    ffi_status(|| {
        if out.is_null() {
            return crate::DATAROUTER_ERR_INVALID_ARGUMENT;
        }
        unsafe { *out = ptr::null_mut() };
        if options.is_null()
            || options_size != std::mem::size_of::<datarouter_frontend_options_v3>()
            || abi_version != DATAROUTER_FRONTEND_ABI_VERSION_V3
        {
            return crate::DATAROUTER_ERR_PROTOCOL;
        }
        let options = match unsafe { frontend_options_from_c_v3(options) } {
            Ok(options) => options,
            Err(code) => return code,
        };
        let frontend = match options.1 {
            DATAROUTER_FRONTEND_DATA_PLANE_DIRECT => Frontend::connect(options.0),
            DATAROUTER_FRONTEND_DATA_PLANE_ROUTED => Frontend::connect_routed(options.0),
            _ => return crate::DATAROUTER_ERR_INVALID_ARGUMENT,
        };
        match frontend {
            Ok(frontend) => {
                let handle = Box::new(datarouter_frontend { frontend });
                unsafe { *out = Box::into_raw(handle) };
                crate::DATAROUTER_OK
            }
            Err(code) => code,
        }
    })
}

#[unsafe(no_mangle)]
/// Peek the next RX frame's payload length without consuming it.
/// Returns `DATAROUTER_ERR_AGAIN` when the RX ring is empty. Useful to size a
/// `datarouter_frontend_recv` buffer before receiving (direct data plane).
///
/// # Safety
/// `frontend` and `out_len` must be valid. Close must not run concurrently.
pub unsafe extern "C" fn datarouter_frontend_frame_len(
    frontend: *mut datarouter_frontend,
    out_len: *mut usize,
) -> c_int {
    ffi_status(|| {
        if frontend.is_null() || out_len.is_null() {
            return crate::DATAROUTER_ERR_INVALID_ARGUMENT;
        }
        match unsafe { &*frontend }.frontend.frame_len() {
            Ok(Some(len)) => {
                unsafe { *out_len = len };
                crate::DATAROUTER_OK
            }
            Ok(None) => crate::DATAROUTER_ERR_AGAIN,
            Err(code) => code,
        }
    })
}

#[unsafe(no_mangle)]
/// Send one packed protocol message.
///
/// # Safety
/// `frontend` must be a live handle, `target_service` a NUL-terminated string,
/// and `data` readable for exactly `data_len` bytes. Close must not run
/// concurrently with this call.
pub unsafe extern "C" fn datarouter_frontend_send(
    frontend: *mut datarouter_frontend,
    target_service: *const c_char,
    data: *const c_void,
    data_len: usize,
    timeout_ms: u32,
) -> c_int {
    ffi_status(|| {
        if frontend.is_null()
            || target_service.is_null()
            || data.is_null()
            || data_len == 0
            || data_len > isize::MAX as usize
        {
            return crate::DATAROUTER_ERR_INVALID_ARGUMENT;
        }
        let target = match unsafe { read_c_string(target_service) } {
            Ok(target) if !target.is_empty() => target,
            _ => return crate::DATAROUTER_ERR_INVALID_ARGUMENT,
        };
        let packed = unsafe { slice::from_raw_parts(data.cast::<u8>(), data_len) };
        match unsafe { &*frontend }
            .frontend
            .send(target.as_str(), packed, timeout_ms)
        {
            Ok(()) => crate::DATAROUTER_OK,
            Err(code) => code,
        }
    })
}

#[unsafe(no_mangle)]
/// Receive one packed protocol message without dropping it on a short buffer.
///
/// # Safety
/// `frontend` and `out_info` must be valid. `out_buf` must be writable for
/// `out_capacity` bytes when the capacity is non-zero; it may be NULL only
/// when capacity is zero. Close must not run concurrently with this call.
/// On `DATAROUTER_ERR_PAYLOAD_TOO_LARGE`, `out_info->len` contains the required
/// capacity and the same queued message remains available for retry.
pub unsafe extern "C" fn datarouter_frontend_recv(
    frontend: *mut datarouter_frontend,
    out_buf: *mut c_void,
    out_capacity: usize,
    out_info: *mut crate::datarouter_message_info,
    timeout_ms: u32,
) -> c_int {
    ffi_status(|| {
        if frontend.is_null()
            || out_info.is_null()
            || out_capacity > isize::MAX as usize
            || (out_capacity != 0 && out_buf.is_null())
        {
            return crate::DATAROUTER_ERR_INVALID_ARGUMENT;
        }
        unsafe {
            *out_info = crate::datarouter_message_info {
                source: [0; crate::DATAROUTER_SERVICE_NAME_MAX + 1],
                len: 0,
            };
        }
        let out: &mut [u8] = if out_capacity == 0 {
            &mut []
        } else {
            unsafe { slice::from_raw_parts_mut(out_buf.cast::<u8>(), out_capacity) }
        };
        match unsafe { &*frontend }.frontend.recv_packed(out, timeout_ms) {
            Ok(info) => {
                unsafe { *out_info = info };
                crate::DATAROUTER_OK
            }
            Err(RecvPackedError::BufferTooSmall { required }) => {
                unsafe { (*out_info).len = required };
                crate::DATAROUTER_ERR_PAYLOAD_TOO_LARGE
            }
            Err(RecvPackedError::Code(code)) => code,
        }
    })
}

#[unsafe(no_mangle)]
/// Snapshot registry entries.
///
/// # Safety
/// `frontend` and `count` must be valid. `entries` must be writable for
/// `capacity` entries, or may be NULL when capacity is zero.
pub unsafe extern "C" fn datarouter_frontend_registry_snapshot(
    frontend: *mut datarouter_frontend,
    entries: *mut crate::datarouter_registry_snapshot_entry,
    capacity: usize,
    count: *mut usize,
) -> c_int {
    ffi_status(|| {
        if frontend.is_null()
            || count.is_null()
            || capacity > isize::MAX as usize
            || (capacity != 0 && entries.is_null())
        {
            return crate::DATAROUTER_ERR_INVALID_ARGUMENT;
        }
        let entries = if capacity == 0 {
            &mut []
        } else {
            unsafe { std::slice::from_raw_parts_mut(entries, capacity) }
        };
        unsafe { &*frontend }
            .frontend
            .registry_snapshot(entries, unsafe { &mut *count })
    })
}

#[unsafe(no_mangle)]
/// Snapshot frontend counters.
///
/// # Safety
/// This legacy entry point does not dereference either pointer and reports an
/// ABI mismatch.
pub unsafe extern "C" fn datarouter_frontend_stats_snapshot(
    _frontend: *mut datarouter_frontend,
    _out_stats: *mut datarouter_frontend_stats,
) -> c_int {
    crate::DATAROUTER_ERR_PROTOCOL
}

#[unsafe(no_mangle)]
/// Snapshot frontend counters using the size-checked frontend ABI.
///
/// # Safety
/// When the size and ABI version match, `frontend` must be a live handle and
/// `out_stats` must point to `out_size` writable bytes. A size/version mismatch
/// returns protocol error before either pointer is dereferenced.
pub unsafe extern "C" fn datarouter_frontend_stats_snapshot_v2(
    frontend: *mut datarouter_frontend,
    out_stats: *mut datarouter_frontend_stats,
    out_size: usize,
    abi_version: u32,
) -> c_int {
    ffi_status(|| {
        if out_size != std::mem::size_of::<datarouter_frontend_stats>()
            || abi_version != DATAROUTER_FRONTEND_ABI_VERSION
        {
            return crate::DATAROUTER_ERR_PROTOCOL;
        }
        if frontend.is_null() || out_stats.is_null() {
            return crate::DATAROUTER_ERR_INVALID_ARGUMENT;
        }
        unsafe { *out_stats = (&*frontend).frontend.stats_snapshot() };
        crate::DATAROUTER_OK
    })
}

#[unsafe(no_mangle)]
/// Close a frontend handle.
///
/// # Safety
/// `frontend` must be NULL or a handle returned by `datarouter_frontend_open`
/// that has not already been closed. No send, receive, or snapshot operation
/// may access the handle concurrently or after this call begins.
pub unsafe extern "C" fn datarouter_frontend_close(frontend: *mut datarouter_frontend) {
    if !frontend.is_null() {
        let _ = panic::catch_unwind(AssertUnwindSafe(|| {
            drop(unsafe { Box::from_raw(frontend) });
        }));
    }
}

unsafe fn frontend_options_from_c(
    options: *const datarouter_frontend_options,
) -> Result<FrontendOptions, c_int> {
    let options = unsafe { *options };
    Ok(FrontendOptions {
        shm_name: unsafe { read_c_string(options.shm_name)? },
        service_name: unsafe { read_c_string(options.service_name)? },
        auth_token: None,
        connect_timeout_ms: options.connect_timeout_ms,
        recv_timeout_ms: options.recv_timeout_ms,
        ack_timeout_ms: options.ack_timeout_ms,
        reader_wait_ms: options.reader_wait_ms,
        app_queue_capacity: options.app_queue_capacity,
        max_message_bytes: options.max_message_bytes,
        max_pending_acks: options.max_pending_acks,
        chunk_ttl_ms: options.chunk_ttl_ms,
        max_reassembly_groups: options.max_reassembly_groups,
        max_reassembly_bytes: options.max_reassembly_bytes,
    })
}

/// `(options, data_plane)` from the v3 struct.
unsafe fn frontend_options_from_c_v3(
    options: *const datarouter_frontend_options_v3,
) -> Result<(FrontendOptions, u8), c_int> {
    let options = unsafe { *options };
    let auth_token = unsafe { read_c_string(options.auth_token)? };
    let base = FrontendOptions {
        shm_name: unsafe { read_c_string(options.shm_name)? },
        service_name: unsafe { read_c_string(options.service_name)? },
        auth_token: if auth_token.is_empty() {
            None
        } else {
            Some(auth_token)
        },
        connect_timeout_ms: options.connect_timeout_ms,
        recv_timeout_ms: options.recv_timeout_ms,
        ack_timeout_ms: options.ack_timeout_ms,
        reader_wait_ms: options.reader_wait_ms,
        app_queue_capacity: options.app_queue_capacity,
        max_message_bytes: options.max_message_bytes,
        max_pending_acks: options.max_pending_acks,
        chunk_ttl_ms: options.chunk_ttl_ms,
        max_reassembly_groups: options.max_reassembly_groups,
        max_reassembly_bytes: options.max_reassembly_bytes,
    };
    Ok((base, options.data_plane))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[repr(C)]
    struct Guarded<const N: usize> {
        before: u64,
        bytes: [u8; N],
        after: u64,
    }

    #[test]
    fn legacy_entry_points_report_abi_mismatch_without_touching_small_buffers() {
        let mut out = std::ptr::NonNull::<datarouter_frontend>::dangling().as_ptr();
        let legacy_options = Guarded::<32> {
            before: 0x1122_3344_5566_7788,
            bytes: [0x5a; 32],
            after: 0x8877_6655_4433_2211,
        };
        let rc = unsafe {
            datarouter_frontend_open(
                legacy_options
                    .bytes
                    .as_ptr()
                    .cast::<datarouter_frontend_options>(),
                &mut out,
            )
        };
        assert_eq!(rc, crate::DATAROUTER_ERR_PROTOCOL);
        assert!(out.is_null());
        assert_eq!(legacy_options.before, 0x1122_3344_5566_7788);
        assert_eq!(legacy_options.after, 0x8877_6655_4433_2211);

        let mut legacy_stats = Guarded::<64> {
            before: 0x0102_0304_0506_0708,
            bytes: [0xa5; 64],
            after: 0x0807_0605_0403_0201,
        };
        let before = legacy_stats.bytes;
        let rc = unsafe {
            datarouter_frontend_stats_snapshot(
                std::ptr::NonNull::<datarouter_frontend>::dangling().as_ptr(),
                legacy_stats
                    .bytes
                    .as_mut_ptr()
                    .cast::<datarouter_frontend_stats>(),
            )
        };
        assert_eq!(rc, crate::DATAROUTER_ERR_PROTOCOL);
        assert_eq!(legacy_stats.bytes, before);
        assert_eq!(legacy_stats.before, 0x0102_0304_0506_0708);
        assert_eq!(legacy_stats.after, 0x0807_0605_0403_0201);
    }

    #[test]
    fn v2_entry_points_validate_size_and_version_before_dereferencing() {
        let mut out = std::ptr::NonNull::<datarouter_frontend>::dangling().as_ptr();
        let options = unsafe { std::mem::zeroed::<datarouter_frontend_options>() };
        let rc = unsafe {
            datarouter_frontend_open_v2(
                &options,
                std::mem::size_of_val(&options),
                DATAROUTER_FRONTEND_ABI_VERSION + 1,
                &mut out,
            )
        };
        assert_eq!(rc, crate::DATAROUTER_ERR_PROTOCOL);
        assert!(out.is_null());

        let mut short = Guarded::<8> {
            before: 0x1111_2222_3333_4444,
            bytes: [0x3c; 8],
            after: 0x4444_3333_2222_1111,
        };
        let before = short.bytes;
        let rc = unsafe {
            datarouter_frontend_stats_snapshot_v2(
                std::ptr::NonNull::<datarouter_frontend>::dangling().as_ptr(),
                short.bytes.as_mut_ptr().cast::<datarouter_frontend_stats>(),
                short.bytes.len(),
                DATAROUTER_FRONTEND_ABI_VERSION,
            )
        };
        assert_eq!(rc, crate::DATAROUTER_ERR_PROTOCOL);
        assert_eq!(short.bytes, before);
        assert_eq!(short.before, 0x1111_2222_3333_4444);
        assert_eq!(short.after, 0x4444_3333_2222_1111);
    }
}
