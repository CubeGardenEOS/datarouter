//! Cargo package for the BlinkCMS datarouter C ABI.
//!
//! This crate compiles and links the C port library as a static native library.
//! The `datarouter-switch` process is still built and run separately; clients
//! use these FFI bindings to open the switch-owned shared memory and log on.
//!
//! The `protocol` module provides the binary message codec for IPC, including
//! message types, TLV encoding, chunking, and business message builders.
//! The `protocol_ffi` module exports these as C ABI functions for Go/Python.

pub mod frontend;
pub mod logging;
pub mod protocol;
mod protocol_ffi;
pub mod query;

use std::ffi::CStr;
use std::os::raw::{c_char, c_int, c_void};

pub const DATAROUTER_OK: c_int = 0;
pub const DATAROUTER_ERR_INVALID_ARGUMENT: c_int = 1;
pub const DATAROUTER_ERR_NOT_FOUND: c_int = 2;
pub const DATAROUTER_ERR_BUSY: c_int = 3;
pub const DATAROUTER_ERR_FULL: c_int = 4;
pub const DATAROUTER_ERR_PAYLOAD_TOO_LARGE: c_int = 5;
pub const DATAROUTER_ERR_AGAIN: c_int = 6;
pub const DATAROUTER_ERR_TIMEOUT: c_int = 7;
pub const DATAROUTER_ERR_PERMISSION: c_int = 8;
pub const DATAROUTER_ERR_PLATFORM: c_int = 9;
pub const DATAROUTER_ERR_PROTOCOL: c_int = 10;
pub const DATAROUTER_ERR_INTERNAL: c_int = 11;

pub const DATAROUTER_MAX_PORTS: usize = 64;
pub const DATAROUTER_DEFAULT_MAX_PORTS: usize = 4;
pub const DATAROUTER_CHANNEL_COUNT: usize = DATAROUTER_DEFAULT_MAX_PORTS * 2;
include!(concat!(env!("OUT_DIR"), "/generated_constants.rs"));
pub const DATAROUTER_SERVICE_NAME_MAX: usize = 31;
pub const DATAROUTER_DEFAULT_TIMEOUT_MS: u32 = 3000;

pub const DATAROUTER_DEFAULT_SHM_NAME: &[u8] = b"/datarouter\0";

#[repr(C)]
pub struct datarouter_port {
    _private: [u8; 0],
}

#[repr(C)]
#[derive(Debug, Copy, Clone)]
pub struct datarouter_options {
    pub shm_name: *const c_char,
    pub timeout_ms: u32,
}

impl Default for datarouter_options {
    fn default() -> Self {
        Self {
            shm_name: DATAROUTER_DEFAULT_SHM_NAME.as_ptr().cast(),
            timeout_ms: DATAROUTER_DEFAULT_TIMEOUT_MS,
        }
    }
}

#[repr(C)]
#[derive(Debug, Copy, Clone)]
pub struct datarouter_message_info {
    pub source: [c_char; DATAROUTER_SERVICE_NAME_MAX + 1],
    pub len: usize,
}

#[repr(C)]
#[derive(Debug, Copy, Clone)]
pub struct datarouter_registry_snapshot_entry {
    pub service_name: [c_char; DATAROUTER_SERVICE_NAME_MAX + 1],
    pub pid: i32,
    pub route_id: u32,
    pub channel_id: u32,
    pub slot: u32,
    pub last_seen_ns: u64,
}

#[repr(C)]
#[derive(Debug, Copy, Clone, Default)]
pub struct datarouter_transport_stats {
    pub corrupted_rings: u64,
    pub registration_recoveries: u64,
    pub blocked_wakeups: u64,
    pub registration_request_recoveries: u64,
    pub dead_process_reaps: u64,
    pub transport_generation: u64,
}

pub type DatarouterDrainCallback = unsafe extern "C" fn(
    info: *const datarouter_message_info,
    data: *const c_void,
    len: usize,
    user_data: *mut c_void,
) -> c_int;

unsafe extern "C" {
    pub fn datarouter_version() -> *const c_char;
    pub fn datarouter_strerror(code: c_int) -> *const c_char;
    pub fn datarouter_open(
        options: *const datarouter_options,
        out: *mut *mut datarouter_port,
    ) -> c_int;
    pub fn datarouter_logon(
        port: *mut datarouter_port,
        service_name: *const c_char,
        errbuf: *mut c_char,
        errbuf_len: usize,
    ) -> c_int;
    pub fn datarouter_send(
        port: *mut datarouter_port,
        target_service: *const c_char,
        data: *const c_void,
        len: usize,
    ) -> c_int;
    pub fn datarouter_recv(
        port: *mut datarouter_port,
        buffer: *mut c_void,
        capacity: usize,
        info: *mut datarouter_message_info,
    ) -> c_int;
    pub fn datarouter_poll(port: *mut datarouter_port) -> c_int;
    pub fn datarouter_touch(port: *mut datarouter_port) -> c_int;
    pub fn datarouter_wait(port: *mut datarouter_port, timeout_ms: u32) -> c_int;
    pub fn datarouter_drain(
        port: *mut datarouter_port,
        budget_us: u32,
        max_frames: usize,
        callback: Option<DatarouterDrainCallback>,
        user_data: *mut c_void,
        drained: *mut usize,
    ) -> c_int;
    pub fn datarouter_registry_snapshot(
        port: *mut datarouter_port,
        entries: *mut datarouter_registry_snapshot_entry,
        capacity: usize,
        count: *mut usize,
    ) -> c_int;
    pub fn datarouter_ttl_ms(port: *mut datarouter_port) -> u32;
    pub fn datarouter_transport_stats_snapshot(
        port: *mut datarouter_port,
        out: *mut datarouter_transport_stats,
    ) -> c_int;
    pub fn datarouter_close(port: *mut datarouter_port);
}

/// Converts a datarouter status code to the C library's static error string.
pub fn strerror(code: c_int) -> &'static CStr {
    unsafe {
        let ptr = datarouter_strerror(code);
        if ptr.is_null() {
            c"unknown error"
        } else {
            CStr::from_ptr(ptr)
        }
    }
}

/// Returns the linked C datarouter library version.
pub fn version() -> &'static CStr {
    unsafe {
        let ptr = datarouter_version();
        if ptr.is_null() {
            c"unknown"
        } else {
            CStr::from_ptr(ptr)
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::path::{Path, PathBuf};

    #[test]
    fn exposes_version_and_error_strings() {
        assert!(!version().to_bytes().is_empty());
        assert_eq!(strerror(DATAROUTER_OK).to_bytes(), b"ok");
    }

    #[test]
    fn option_default_matches_c_defaults() {
        let options = datarouter_options::default();
        assert_eq!(options.timeout_ms, DATAROUTER_DEFAULT_TIMEOUT_MS);
        assert!(!options.shm_name.is_null());
    }

    #[test]
    fn application_code_does_not_use_raw_port_api() {
        let manifest_dir = PathBuf::from(env!("CARGO_MANIFEST_DIR"));
        let repo_root = manifest_dir
            .parent()
            .and_then(Path::parent)
            .expect("repo root");
        let forbidden = [
            "datarouter_open",
            "datarouter_logon",
            "datarouter_send",
            "datarouter_recv",
            "datarouter_wait",
            "datarouter_drain",
            "datarouter_poll",
            "datarouter_close",
        ];
        let mut violations = Vec::new();
        for dir in ["core", "agent", "frontend"] {
            scan_source_dir(&repo_root.join(dir), &forbidden, &mut violations);
        }
        assert!(
            violations.is_empty(),
            "application code must use datarouter_frontend_* or SDK abstractions, not raw port API:\n{}",
            violations.join("\n")
        );
    }

    fn scan_source_dir(path: &Path, forbidden: &[&str], violations: &mut Vec<String>) {
        let Ok(entries) = std::fs::read_dir(path) else {
            return;
        };
        for entry in entries.flatten() {
            let path = entry.path();
            let file_name = entry.file_name();
            let name = file_name.to_string_lossy();
            if path.is_dir() {
                if matches!(
                    name.as_ref(),
                    ".git" | ".codegraph" | "build" | "dist" | "node_modules" | "target"
                ) {
                    continue;
                }
                scan_source_dir(&path, forbidden, violations);
                continue;
            }
            if !is_source_file(&path) {
                continue;
            }
            let Ok(text) = std::fs::read_to_string(&path) else {
                continue;
            };
            for needle in forbidden {
                if text.contains(needle) {
                    violations.push(format!("{} contains {needle}", path.display()));
                }
            }
        }
    }

    fn is_source_file(path: &Path) -> bool {
        matches!(
            path.extension().and_then(|ext| ext.to_str()),
            Some("go" | "rs" | "py" | "ts" | "tsx" | "js" | "jsx" | "vue")
        )
    }
}
