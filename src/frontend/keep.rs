//! KEEP region frontend — thin FFI delegation to the C single source of
//! truth (`src/keep.c`). No hash/scan/validation is implemented in Rust.
//!
//! Copying reads hold a C lease through memcpy. `get_ref` holds an exclusive
//! read lease until `verify_ref` consumes it; concurrent writes return Again.
//! The tuple adapter retains the mapping until verify_ref; callers must verify
//! exactly once to release it. Never retain a pointer after verification.
//! Prefer KeepReference for a lifetime checked guard with automatic release.

use std::ffi::CString;
use std::os::raw::{c_void, c_int};
use std::ptr;
use std::collections::HashMap;
use std::sync::{Arc, Mutex, OnceLock};
use std::sync::atomic::{AtomicU64, Ordering};
use crate::frontend::port_frontend::PortOwner;

type LeaseKey = (usize, u64);
struct LegacyLease {
    owner: Arc<PortOwner>,
    _scope: Option<Arc<()>>,
    c_seq: u64,
}
fn legacy_leases() -> &'static Mutex<HashMap<LeaseKey, LegacyLease>> {
    static LEASES: OnceLock<Mutex<HashMap<LeaseKey, LegacyLease>>> = OnceLock::new();
    LEASES.get_or_init(|| Mutex::new(HashMap::new()))
}

/// An immutable KEEP value with an owned mapping and C read lease. Dropping
/// the guard releases the lease; it may outlive the frontend that acquired it.
pub struct KeepReference {
    owner: Arc<PortOwner>,
    value: *const u8,
    len: usize,
    seq: u64,
}
// C prevents every writer from overwriting this mapping until this guard's
// single release. The process identity is shared across its threads.
unsafe impl Send for KeepReference {}
unsafe impl Sync for KeepReference {}
impl AsRef<[u8]> for KeepReference {
    fn as_ref(&self) -> &[u8] {
        // C returns a mapped arena address even for empty values.
        unsafe { std::slice::from_raw_parts(self.value, self.len) }
    }
}
impl Drop for KeepReference {
    fn drop(&mut self) {
        unsafe { crate::datarouter_keep_verify_ref(self.owner.port, self.seq); }
    }
}

pub fn keep_reference(port: &PortFrontend, key: &[u8]) -> Result<KeepReference, KeepStatus> {
    let (value, len, seq) = unsafe { acquire_ref(port, key) }?;
    Ok(KeepReference { owner: port.keep_owner(), value, len, seq })
}

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
/// Finish reading before calling `keep_verify_ref` and never dereference the
/// pointer afterward. Verify before dropping the frontend to release the
/// retained mapping; forgetting verification leaks the mapping and read lease.
/// An outstanding reference blocks KEEP operations with `Again`.
pub unsafe fn keep_get_ref(
    port: &PortFrontend,
    key: &[u8],
) -> Result<(*const u8, usize, u64), KeepStatus> {
    unsafe { keep_get_ref_scoped(port, key, None) }
}

pub(crate) unsafe fn keep_get_ref_scoped(
    port: &PortFrontend, key: &[u8], scope: Option<Arc<()>>,
) -> Result<(*const u8, usize, u64), KeepStatus> {
    static NEXT_TOKEN: AtomicU64 = AtomicU64::new(1);
    let token = NEXT_TOKEN.fetch_update(Ordering::Relaxed, Ordering::Relaxed, |n| n.checked_add(1))
        .map_err(|_| KeepStatus::Internal)?;
    let (value, len, c_seq) = unsafe { acquire_ref(port, key) }?;
    let owner_key = scope.as_ref().map_or(port.raw() as usize, |s| Arc::as_ptr(s) as usize);
    // Tokens remain distinct even when a reconnected mapping restarts its C
    // generation. Verification releases the original mapping, not the new port.
    legacy_leases().lock().unwrap_or_else(|p| p.into_inner())
        .insert((owner_key, token), LegacyLease { owner: port.keep_owner(), _scope: scope, c_seq });
    Ok((value, len, token))
}

unsafe fn acquire_ref(port: &PortFrontend, key: &[u8]) -> Result<(*const u8, usize, u64), KeepStatus> {
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
    verify_owned_lease(port.raw() as usize, seq)
}

pub(crate) fn keep_verify_ref_scoped(scope: &Arc<()>, seq: u64) -> Result<(), KeepStatus> {
    verify_owned_lease(Arc::as_ptr(scope) as usize, seq)
}

fn verify_owned_lease(owner_key: usize, seq: u64) -> Result<(), KeepStatus> {
    // Safe callers cannot release an RAII guard by guessing its C sequence.
    let lease = legacy_leases().lock().unwrap_or_else(|p| p.into_inner())
        .remove(&(owner_key, seq)).ok_or(KeepStatus::Again)?;
    let code = unsafe { crate::datarouter_keep_verify_ref(lease.owner.port, lease.c_seq) };
    // Drop the mapping reference on every completed verification, including a
    // disconnected old mapping. A consumed token can never release twice.
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
        // Lock contention: retry the complete operation.
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
