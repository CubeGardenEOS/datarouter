use std::ffi::CStr;
use std::os::raw::{c_char, c_int};

pub const DEFAULT_CONNECT_TIMEOUT_MS: u32 = 3000;
pub const DEFAULT_RECV_TIMEOUT_MS: u32 = 3000;
pub const DEFAULT_ACK_TIMEOUT_MS: u32 = 2000;
pub const DEFAULT_READER_WAIT_MS: u32 = 100;
pub const DEFAULT_APP_QUEUE_CAPACITY: usize = 256;
pub const DEFAULT_MAX_MESSAGE_BYTES: usize = 64 * 1024 * 1024;
pub const DEFAULT_MAX_PENDING_ACKS: usize = 4096;
pub const DEFAULT_CHUNK_TTL_MS: u32 = 30_000;
pub const DEFAULT_MAX_REASSEMBLY_GROUPS: usize = 1024;
pub const DEFAULT_MAX_REASSEMBLY_BYTES: usize = 128 * 1024 * 1024;
pub const MAX_READER_WAIT_MS: u32 = 1000;
pub const MAX_CHUNK_TTL_MS: u32 = 300_000;

#[derive(Debug, Clone)]
pub struct FrontendOptions {
    pub shm_name: String,
    pub service_name: String,
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

impl Default for FrontendOptions {
    fn default() -> Self {
        Self {
            shm_name: String::from_utf8_lossy(
                &crate::DATAROUTER_DEFAULT_SHM_NAME[..crate::DATAROUTER_DEFAULT_SHM_NAME.len() - 1],
            )
            .into_owned(),
            service_name: String::new(),
            connect_timeout_ms: DEFAULT_CONNECT_TIMEOUT_MS,
            recv_timeout_ms: DEFAULT_RECV_TIMEOUT_MS,
            ack_timeout_ms: DEFAULT_ACK_TIMEOUT_MS,
            reader_wait_ms: DEFAULT_READER_WAIT_MS,
            app_queue_capacity: DEFAULT_APP_QUEUE_CAPACITY,
            max_message_bytes: DEFAULT_MAX_MESSAGE_BYTES,
            max_pending_acks: DEFAULT_MAX_PENDING_ACKS,
            chunk_ttl_ms: DEFAULT_CHUNK_TTL_MS,
            max_reassembly_groups: DEFAULT_MAX_REASSEMBLY_GROUPS,
            max_reassembly_bytes: DEFAULT_MAX_REASSEMBLY_BYTES,
        }
    }
}

impl FrontendOptions {
    pub fn normalized(mut self) -> Result<Self, c_int> {
        let defaults = Self::default();
        if self.shm_name.is_empty() {
            self.shm_name = defaults.shm_name;
        }
        if self.service_name.is_empty() {
            return Err(crate::DATAROUTER_ERR_INVALID_ARGUMENT);
        }
        if self.connect_timeout_ms == 0 {
            self.connect_timeout_ms = defaults.connect_timeout_ms;
        }
        if self.recv_timeout_ms == 0 {
            self.recv_timeout_ms = defaults.recv_timeout_ms;
        }
        if self.ack_timeout_ms == 0 {
            self.ack_timeout_ms = defaults.ack_timeout_ms;
        }
        if self.reader_wait_ms == 0 {
            self.reader_wait_ms = defaults.reader_wait_ms;
        }
        if self.app_queue_capacity == 0 {
            self.app_queue_capacity = defaults.app_queue_capacity;
        }
        if self.max_message_bytes == 0 {
            self.max_message_bytes = defaults.max_message_bytes;
        }
        if self.max_pending_acks == 0 {
            self.max_pending_acks = defaults.max_pending_acks;
        }
        if self.chunk_ttl_ms == 0 {
            self.chunk_ttl_ms = defaults.chunk_ttl_ms;
        }
        if self.max_reassembly_groups == 0 {
            self.max_reassembly_groups = defaults.max_reassembly_groups;
        }
        if self.max_reassembly_bytes == 0 {
            self.max_reassembly_bytes = defaults.max_reassembly_bytes;
        }
        if self.max_message_bytes < crate::protocol::MAX_APP_PAYLOAD {
            return Err(crate::DATAROUTER_ERR_INVALID_ARGUMENT);
        }
        if self.reader_wait_ms > MAX_READER_WAIT_MS || self.chunk_ttl_ms > MAX_CHUNK_TTL_MS {
            return Err(crate::DATAROUTER_ERR_INVALID_ARGUMENT);
        }
        Ok(self)
    }
}

/// Read a nullable, NUL-terminated C string into an owned Rust string.
///
/// # Safety
/// A non-null `ptr` must point to a valid NUL-terminated byte sequence for the
/// duration of this call.
pub unsafe fn read_c_string(ptr: *const c_char) -> Result<String, c_int> {
    if ptr.is_null() {
        return Ok(String::new());
    }
    unsafe { CStr::from_ptr(ptr) }
        .to_str()
        .map(|value| value.to_owned())
        .map_err(|_| crate::DATAROUTER_ERR_INVALID_ARGUMENT)
}

pub fn normalize_timeout(value: u32, default_value: u32) -> u32 {
    if value == 0 { default_value } else { value }
}
