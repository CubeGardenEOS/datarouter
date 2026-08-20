//! KEEP region frontend — thin FFI delegation to the C single source of
//! truth (`src/keep.c`). No hash/scan/validation is implemented in Rust.
//!
//! The C API is one-copy on the copying `get` and zero-copy on the two-phase
//! `get_ref`/`verify_ref`. `get_ref` returns a POINTER into shared memory
//! guarded by a seqlock epoch; the caller must copy/deliver the bytes and then
//! call `verify_ref(seq)` before using the result (data may tear otherwise).
//! Never hold a `get_ref` pointer across an async boundary.

use std::ffi::CString;
use std::os::raw::{c_void, c_int};
use std::ptr;

use crate::frontend::port_frontend::PortFrontend;

/// Writable-visible status of a KEEP operation (maps each C error code to a
/// stable value callers can match on without importing every raw constant).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum KeepStatus {
    Ok,
    NotFound,
    Full,
    Again,
    Permission,
    InvalidArgument,
    Protocol,
    Internal,
    Other(c_int),
}

impl KeepStatus {
    pub fn from_code(code: c_int) -> Self {
        match code {
            crate::DATAROUTER_OK => KeepStatus::Ok,
            crate::DATAROUTER_ERR_NOT_FOUND => KeepStatus::NotFound,
            crate::DATAROUTER_ERR_FULL => KeepStatus::Full,
            crate::DATAROUTER_ERR_AGAIN => KeepStatus::Again,
            crate::DATAROUTER_ERR_PERMISSION => KeepStatus::Permission,
            crate::DATAROUTER_ERR_INVALID_ARGUMENT => KeepStatus::InvalidArgument,
            crate::DATAROUTER_ERR_PROTOCOL => KeepStatus::Protocol,
            crate::DATAROUTER_ERR_INTERNAL => KeepStatus::Internal,
            other => KeepStatus::Other(other),
        }
    }
}

/// Copying read (one memcpy: arena -> caller buffer).
pub fn keep_get(
    port: &PortFrontend,
    key: &[u8],
    out: &mut [u8],
) -> Result<usize, KeepStatus> {
    let (key_ptr, key_len) = ptr_pair(key);
    let mut out_len = 0usize;
    // SAFETY: out is a live mutable slice; C writes at most out.len() bytes
    // and reports the real length in out_len.
    let code = unsafe {
        crate::datarouter_keep_get(
            port.raw(),
            key_ptr,
            key_len,
            out.as_mut_ptr().cast::<c_void>(),
            out.len(),
            &mut out_len,
        )
    };
    match code {
        crate::DATAROUTER_OK => Ok(out_len),
        other => Err(KeepStatus::from_code(other)),
    }
}

/// Two-phase zero-copy read. On success returns (value ptr, len, seq). The
/// returned pointer borrows shared memory; call [`keep_verify_ref`] with `seq`
/// after copying/delivering the value. Pointer must not outlive the read.
///
/// # Safety
/// The returned raw pointer is valid only until a concurrent writer mutates
/// the region; it is NOT safe to dereference after returning. Copy the bytes
/// immediately and call `keep_verify_ref(seq)`.
pub unsafe fn keep_get_ref(
    port: &PortFrontend,
    key: &[u8],
) -> Result<(*const u8, usize, u64), KeepStatus> {
    let (key_ptr, key_len) = ptr_pair(key);
    let mut value: *const c_void = ptr::null();
    let mut value_len = 0usize;
    let mut seq = 0u64;
    let code = unsafe {
        crate::datarouter_keep_get_ref(
            port.raw(),
            key_ptr,
            key_len,
            &mut value,
            &mut value_len,
            &mut seq,
        )
    };
    match code {
        crate::DATAROUTER_OK => Ok((value.cast::<u8>(), value_len, seq)),
        other => Err(KeepStatus::from_code(other)),
    }
}

pub fn keep_verify_ref(port: &PortFrontend, seq: u64) -> Result<(), KeepStatus> {
    let code = unsafe { crate::datarouter_keep_verify_ref(port.raw(), seq) };
    match code {
        crate::DATAROUTER_OK => Ok(()),
        other => Err(KeepStatus::from_code(other)),
    }
}

pub fn keep_put(port: &PortFrontend, key: &[u8], value: &[u8], ttl_ns: u64) -> Result<(), KeepStatus> {
    let (key_ptr, key_len) = ptr_pair(key);
    let (val_ptr, val_len) = ptr_pair(value);
    let code = unsafe {
        crate::datarouter_keep_put(port.raw(), key_ptr, key_len, val_ptr, val_len, ttl_ns)
    };
    match code {
        crate::DATAROUTER_OK => Ok(()),
        other => Err(KeepStatus::from_code(other)),
    }
}

pub fn keep_invalidate(port: &PortFrontend, key: &[u8]) -> Result<(), KeepStatus> {
    let (key_ptr, key_len) = ptr_pair(key);
    let code = unsafe { crate::datarouter_keep_invalidate(port.raw(), key_ptr, key_len) };
    match code {
        crate::DATAROUTER_OK => Ok(()),
        other => Err(KeepStatus::from_code(other)),
    }
}

pub fn keep_invalidate_prefix(port: &PortFrontend, prefix: &[u8]) -> Result<(), KeepStatus> {
    let (prefix_ptr, prefix_len) = ptr_pair(prefix);
    let code = unsafe {
        crate::datarouter_keep_invalidate_prefix(port.raw(), prefix_ptr, prefix_len)
    };
    match code {
        crate::DATAROUTER_OK => Ok(()),
        // partial completion: caller should re-invoke for the remainder.
        crate::DATAROUTER_ERR_AGAIN => Err(KeepStatus::Again),
        other => Err(KeepStatus::from_code(other)),
    }
}

pub fn keep_stats(port: &PortFrontend) -> Result<crate::datarouter_keep_stats_t, KeepStatus> {
    let mut stats = crate::datarouter_keep_stats_t::default();
    let code = unsafe { crate::datarouter_keep_stats(port.raw(), &mut stats) };
    match code {
        crate::DATAROUTER_OK => Ok(stats),
        other => Err(KeepStatus::from_code(other)),
    }
}

pub fn keep_grant(port: &PortFrontend, service_name: &str) -> Result<(), KeepStatus> {
    let name = CString::new(service_name).map_err(|_| KeepStatus::InvalidArgument)?;
    let code = unsafe { crate::datarouter_keep_grant(port.raw(), name.as_ptr()) };
    match code {
        crate::DATAROUTER_OK => Ok(()),
        other => Err(KeepStatus::from_code(other)),
    }
}

fn ptr_pair(data: &[u8]) -> (*const c_void, usize) {
    if data.is_empty() {
        (ptr::null(), 0)
    } else {
        (data.as_ptr().cast::<c_void>(), data.len())
    }
}
