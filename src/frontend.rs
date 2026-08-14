//! Application-facing datarouter frontend.
//!
//! `Frontend` is the only application transport boundary. It owns the raw port
//! read loop and keeps chunking, ACKs, reassembly, queueing, backpressure, and
//! registry access inside Rust.
//!
//! With the default `unsafe` Cargo feature (direct data plane, 2 copies),
//! [`Frontend::connect_unsafe`] skips the reader thread and queue: data moves
//! with [`Frontend::unsafe_send`]/[`Frontend::unsafe_recv`] directly against
//! the target RX ring (2 copies per direction), bypassing the switch's data
//! plane. Build with `--no-default-features` for the safe copy-mode
//! transport (switch routed, 4 copies).

pub mod ffi;
pub mod inbound;
pub mod options;
pub mod outbound;
pub mod port_frontend;
pub mod queue;

#[cfg(debug_assertions)]
use std::collections::HashMap;
use std::os::raw::{c_char, c_int};
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::{Arc, Condvar, Mutex, RwLock};
use std::thread::{self, JoinHandle};
#[cfg(any(feature = "unsafe", debug_assertions))]
use std::time::Instant;
use std::time::{Duration, SystemTime, UNIX_EPOCH};

use crate::protocol;

pub use ffi::{
    datarouter_frontend, datarouter_frontend_open, datarouter_frontend_options,
    datarouter_frontend_recv, datarouter_frontend_registry_snapshot, datarouter_frontend_send,
    datarouter_frontend_stats, datarouter_frontend_stats_snapshot,
};
pub use options::FrontendOptions;

#[derive(Debug, Clone)]
pub struct FrontendMessage {
    pub source: String,
    pub message: protocol::Message,
}

#[derive(Debug, Clone)]
pub struct RegistryDiagnostic {
    pub services: Vec<String>,
    pub summary: String,
}

#[derive(Debug)]
pub struct QueuedMessage {
    pub source: [c_char; crate::DATAROUTER_SERVICE_NAME_MAX + 1],
    pub packed: Vec<u8>,
}

#[derive(Debug, Default)]
pub struct FrontendStats {
    pub sent_frames: AtomicU64,
    pub sent_chunks: AtomicU64,
    pub received_frames: AtomicU64,
    pub received_complete: AtomicU64,
    pub received_chunk_acks: AtomicU64,
    pub received_chunk_errors: AtomicU64,
    pub ack_timeouts: AtomicU64,
    pub queue_full: AtomicU64,
    pub protocol_errors: AtomicU64,
    pub dropped_frames: AtomicU64,
    pub reader_alive: AtomicU64,
    pub reader_errors: AtomicU64,
    pub keepalive_ticks: AtomicU64,
    pub last_keepalive_ns: AtomicU64,
    pub reassembly_expired: AtomicU64,
    pub reassembly_rejected: AtomicU64,
    pub registration_recoveries: AtomicU64,
    pub switch_recoveries: AtomicU64,
    pub last_recovery_error_log_ns: AtomicU64,
}

pub struct FrontendInner {
    port: RwLock<Option<port_frontend::PortFrontend>>,
    pub options: FrontendOptions,
    pub closing: AtomicBool,
    pub ack_waiters: outbound::AckWaiters,
    pub complete_queue: queue::BoundedQueue<QueuedMessage>,
    /// Reusable receive buffers returned by the application side after
    /// draining a queued message. The reader hands each received frame to the
    /// queue by moving a pooled buffer (zero-copy), and `recv`/`recv_packed`
    /// recycle it here, so steady-state reception performs no allocation.
    pub buffer_pool: Mutex<std::collections::VecDeque<Vec<u8>>>,
    pub stats: FrontendStats,
    keepalive_interval: Duration,
    keepalive_wait: Condvar,
    keepalive_wait_state: Mutex<()>,
    #[cfg(debug_assertions)]
    ipc_timings: Mutex<HashMap<String, IpcTiming>>,
}

#[cfg(debug_assertions)]
#[derive(Debug)]
struct IpcTiming {
    target_service: String,
    module: String,
    action: String,
    started: Instant,
}

unsafe impl Send for FrontendInner {}
unsafe impl Sync for FrontendInner {}

pub struct Frontend {
    pub(crate) inner: Arc<FrontendInner>,
    reader: Mutex<Option<JoinHandle<()>>>,
    keepalive: Mutex<Option<JoinHandle<()>>>,
}

impl Frontend {
    pub fn connect(options: FrontendOptions) -> Result<Self, c_int> {
        let options = options.normalized()?;
        let port = port_frontend::PortFrontend::open(&options)?;
        let keepalive_interval = keepalive_interval(port.ttl_ms());
        let inner = Arc::new(FrontendInner {
            complete_queue: queue::BoundedQueue::new(options.app_queue_capacity),
            ack_waiters: outbound::AckWaiters::new(options.max_pending_acks),
            buffer_pool: Mutex::new(std::collections::VecDeque::new()),
            closing: AtomicBool::new(false),
            stats: FrontendStats::default(),
            port: RwLock::new(Some(port)),
            keepalive_interval,
            keepalive_wait: Condvar::new(),
            keepalive_wait_state: Mutex::new(()),
            #[cfg(debug_assertions)]
            ipc_timings: Mutex::new(HashMap::new()),
            options,
        });
        let reader = inbound::spawn_reader(Arc::clone(&inner));
        let keepalive = spawn_keepalive(Arc::clone(&inner));
        Ok(Self {
            inner,
            reader: Mutex::new(Some(reader)),
            keepalive: Mutex::new(Some(keepalive)),
        })
    }

    /// Connect in direct (unsafe) data-plane mode (default feature; build
    /// with `--no-default-features` for the safe copy mode). The port logs on through the switch's control plane as
    /// usual, but **no reader thread is spawned** and the data path uses
    /// [`unsafe_send`](Self::unsafe_send)/[`unsafe_recv`](Self::unsafe_recv):
    /// the sender writes straight into the target's RX ring, so the switch
    /// never touches payload bytes and the full chain costs two copies
    /// (app -> target RX ring, RX ring -> app buffer). The switch keeps only
    /// control-plane duties (register/unregister, TTL/dead reaping) plus
    /// control-request interrupts.
    ///
    /// The keepalive thread still runs so the registration survives TTL.
    /// [`recv`](Self::recv)/[`recv_packed`](Self::recv_packed) are unusable
    /// here (no queue); use the direct methods below.
    #[cfg(feature = "unsafe")]
    pub fn connect_unsafe(options: FrontendOptions) -> Result<Self, c_int> {
        let options = options.normalized()?;
        let port = port_frontend::PortFrontend::open(&options)?;
        let keepalive_interval = keepalive_interval(port.ttl_ms());
        let inner = Arc::new(FrontendInner {
            complete_queue: queue::BoundedQueue::new(options.app_queue_capacity),
            ack_waiters: outbound::AckWaiters::new(options.max_pending_acks),
            buffer_pool: Mutex::new(std::collections::VecDeque::new()),
            closing: AtomicBool::new(false),
            stats: FrontendStats::default(),
            port: RwLock::new(Some(port)),
            keepalive_interval,
            keepalive_wait: Condvar::new(),
            keepalive_wait_state: Mutex::new(()),
            #[cfg(debug_assertions)]
            ipc_timings: Mutex::new(HashMap::new()),
            options,
        });
        let keepalive = spawn_keepalive(Arc::clone(&inner));
        Ok(Self {
            inner,
            reader: Mutex::new(None),
            keepalive: Mutex::new(Some(keepalive)),
        })
    }

    pub fn send(&self, target_service: &str, packed: &[u8], timeout_ms: u32) -> Result<(), c_int> {
        #[cfg(debug_assertions)]
        let tracked_request_id = self.track_ipc_request_timing(target_service, packed);
        let result = outbound::send(&self.inner, target_service, packed, timeout_ms);
        #[cfg(debug_assertions)]
        if result.is_err()
            && let Some(request_id) = tracked_request_id
            && let Ok(mut timings) = self.inner.ipc_timings.lock()
        {
            timings.remove(&request_id);
        }
        result
    }

    /// Direct (unsafe) send: append `data` straight into the target's RX
    /// ring, bypassing the switch's routing loop. No chunking/ACK and no
    /// blocked-queue protocol on this path, so `data` must be a single frame
    /// (≤ `DATAROUTER_MAX_APP_PAYLOAD`); a full target ring returns
    /// `DATAROUTER_ERR_FULL`. On a stale self slot the port is re-registered
    /// and the send retried, matching the normal path.
    #[cfg(feature = "unsafe")]
    pub fn unsafe_send(&self, target_service: &str, data: &[u8]) -> Result<(), c_int> {
        if data.is_empty() {
            return Err(crate::DATAROUTER_ERR_INVALID_ARGUMENT);
        }
        if data.len() > self.inner.options.max_message_bytes {
            return Err(crate::DATAROUTER_ERR_PAYLOAD_TOO_LARGE);
        }
        let result = self.inner.unsafe_send_frame(target_service, data);
        if result.is_ok() {
            self.inner.stats.sent_frames.fetch_add(1, Ordering::Relaxed);
        }
        result
    }

    /// Direct (unsafe) receive: block until a frame arrives, then read it
    /// straight out of our RX ring into `out_buf` (single ring -> buffer
    /// copy; no reader thread, no queue). The frame bytes land in `out_buf`
    /// and the returned info carries `source` + `len`. `Ok(None)` when the
    /// timeout elapses without a frame.
    ///
    /// A frame larger than `out_buf` returns
    /// `DATAROUTER_ERR_PAYLOAD_TOO_LARGE` and is **not consumed**; use
    /// [`frame_len`](Self::frame_len) or a larger buffer and call again.
    /// Concurrent `unsafe_recv` from multiple threads of one frontend is
    /// serialized by the port's RX mutex (safe, but they contend on the
    /// single RX ring; prefer one consumer thread per frontend).
    #[cfg(feature = "unsafe")]
    pub fn unsafe_recv(
        &self,
        out_buf: &mut [u8],
        timeout_ms: u32,
    ) -> Result<Option<crate::datarouter_message_info>, c_int> {
        let timeout_ms =
            options::normalize_timeout(timeout_ms, self.inner.options.recv_timeout_ms);
        let deadline = Instant::now() + Duration::from_millis(u64::from(timeout_ms));
        loop {
            let arrived = match self.inner.with_port(|port| port.unsafe_recv(out_buf))? {
                Ok(Some(info)) => Some(info),
                Ok(None) => None,
                // PAYLOAD_TOO_LARGE (frame kept in the ring) and any other
                // hard error must surface, not masquerade as "no frame".
                Err(code) => return Err(code),
            };
            if let Some(info) = arrived {
                self.inner.stats.received_frames.fetch_add(1, Ordering::Relaxed);
                self.inner.stats.received_complete.fetch_add(1, Ordering::Relaxed);
                return Ok(Some(info));
            }
            let remaining = deadline.saturating_duration_since(Instant::now());
            if remaining.is_zero() {
                return Ok(None);
            }
            let wait_ms = u32::try_from(remaining.as_millis())
                .unwrap_or(u32::MAX)
                .max(1);
            match self.inner.with_port(|port| port.wait(wait_ms))? {
                crate::DATAROUTER_OK => {}
                crate::DATAROUTER_ERR_TIMEOUT | crate::DATAROUTER_ERR_AGAIN => {
                    return Ok(None);
                }
                crate::DATAROUTER_ERR_NOT_FOUND => {
                    if !self.inner.recover_self_after_not_found("unsafe recv wait") {
                        return Err(crate::DATAROUTER_ERR_NOT_FOUND);
                    }
                }
                code => return Err(code),
            }
        }
    }

    /// Direct (unsafe) poll: `Ok(true)` when our RX ring holds a complete
    /// frame, `Ok(false)` when it is empty.
    #[cfg(feature = "unsafe")]
    pub fn unsafe_poll(&self) -> Result<bool, c_int> {
        match self.inner.with_port(|port| port.poll())? {
            crate::DATAROUTER_OK => Ok(true),
            crate::DATAROUTER_ERR_AGAIN | crate::DATAROUTER_ERR_TIMEOUT => Ok(false),
            crate::DATAROUTER_ERR_NOT_FOUND => Err(crate::DATAROUTER_ERR_NOT_FOUND),
            code => Err(code),
        }
    }

    /// Peek the next RX frame's payload length without consuming it
    /// (`Ok(None)` when empty). In direct (unsafe) mode a frame larger than
    /// the caller's `unsafe_recv` buffer returns
    /// `DATAROUTER_ERR_PAYLOAD_TOO_LARGE` **without consuming the frame**;
    /// call this first (or retry with a larger buffer) to size the receive
    /// buffer, then call [`unsafe_recv`](Self::unsafe_recv) again.
    pub fn frame_len(&self) -> Result<Option<usize>, c_int> {
        self.inner.with_port(|port| port.frame_len())?
    }

    pub fn recv(&self, timeout_ms: u32) -> Result<FrontendMessage, c_int> {
        let timeout_ms = options::normalize_timeout(timeout_ms, self.inner.options.recv_timeout_ms);
        let queued = self.inner.complete_queue.recv(timeout_ms)?;
        let message =
            protocol::unpack(&queued.packed).map_err(|_| crate::DATAROUTER_ERR_PROTOCOL)?;
        self.inner.recycle_buffer(queued.packed);
        self.report_ipc_response_timing(&message);
        Ok(FrontendMessage {
            source: c_source_to_string(&queued.source),
            message,
        })
    }

    pub fn recv_packed(
        &self,
        out_buf: &mut [u8],
        timeout_ms: u32,
    ) -> Result<crate::datarouter_message_info, RecvPackedError> {
        let timeout_ms = options::normalize_timeout(timeout_ms, self.inner.options.recv_timeout_ms);
        let queued = match self
            .inner
            .complete_queue
            .recv_fitting(timeout_ms, |message| message.packed.len(), out_buf.len())
            .map_err(RecvPackedError::Code)?
        {
            queue::ReceiveResult::Item(message) => message,
            queue::ReceiveResult::TooLarge { required } => {
                return Err(RecvPackedError::BufferTooSmall { required });
            }
        };
        #[cfg(debug_assertions)]
        if let Ok(message) = protocol::unpack(&queued.packed) {
            self.report_ipc_response_timing(&message);
        }
        out_buf[..queued.packed.len()].copy_from_slice(&queued.packed);
        let info = crate::datarouter_message_info {
            source: queued.source,
            len: queued.packed.len(),
        };
        self.inner.recycle_buffer(queued.packed);
        Ok(info)
    }

    pub fn registry_snapshot(
        &self,
        entries: &mut [crate::datarouter_registry_snapshot_entry],
        count: &mut usize,
    ) -> c_int {
        self.inner
            .with_port(|port| port.registry_snapshot(entries, count))
            .unwrap_or(crate::DATAROUTER_ERR_INTERNAL)
    }

    /// Current switch capacity: `(max_ports, active_ports)`. `max_ports` is
    /// the number of slots the running switch allocated; it grows via
    /// `switch_frontend::SwitchHandle::expand_capacity`.
    pub fn capacity(&self) -> Result<(u32, u32), c_int> {
        self.inner
            .with_port(|port| port.capacity())
            .unwrap_or(Err(crate::DATAROUTER_ERR_INTERNAL))
    }

    pub fn registry_diagnostic(&self, capacity: usize) -> RegistryDiagnostic {
        if capacity == 0 {
            return RegistryDiagnostic {
                services: Vec::new(),
                summary: "empty".to_owned(),
            };
        }
        let mut entries = vec![
            crate::datarouter_registry_snapshot_entry {
                service_name: [0; crate::DATAROUTER_SERVICE_NAME_MAX + 1],
                pid: 0,
                route_id: 0,
                channel_id: 0,
                slot: 0,
                last_seen_ns: 0,
            };
            capacity
        ];
        let mut count = 0usize;
        let rc = self.registry_snapshot(&mut entries, &mut count);
        if rc != crate::DATAROUTER_OK && rc != crate::DATAROUTER_ERR_FULL {
            return RegistryDiagnostic {
                services: Vec::new(),
                summary: format!("unavailable rc={rc}"),
            };
        }
        let limit = count.min(entries.len());
        let mut services = Vec::with_capacity(limit);
        let mut parts = Vec::with_capacity(limit);
        for entry in &entries[..limit] {
            let service = c_source_to_string(&entry.service_name);
            parts.push(format!(
                "{}:slot={},route={},pid={}",
                service, entry.slot, entry.route_id, entry.pid
            ));
            services.push(service);
        }
        RegistryDiagnostic {
            services,
            summary: if parts.is_empty() {
                "empty".to_owned()
            } else {
                parts.join(",")
            },
        }
    }

    pub fn touch(&self) -> Result<(), c_int> {
        self.inner.touch()
    }

    pub fn stats_snapshot(&self) -> ffi::datarouter_frontend_stats {
        let transport = self
            .inner
            .with_port(|port| port.transport_stats())
            .ok()
            .and_then(Result::ok)
            .unwrap_or_default();
        ffi::datarouter_frontend_stats {
            sent_frames: self.inner.stats.sent_frames.load(Ordering::Relaxed),
            sent_chunks: self.inner.stats.sent_chunks.load(Ordering::Relaxed),
            received_frames: self.inner.stats.received_frames.load(Ordering::Relaxed),
            received_complete: self.inner.stats.received_complete.load(Ordering::Relaxed),
            received_chunk_acks: self.inner.stats.received_chunk_acks.load(Ordering::Relaxed),
            received_chunk_errors: self
                .inner
                .stats
                .received_chunk_errors
                .load(Ordering::Relaxed),
            ack_timeouts: self.inner.stats.ack_timeouts.load(Ordering::Relaxed),
            queue_full: self.inner.stats.queue_full.load(Ordering::Relaxed),
            protocol_errors: self.inner.stats.protocol_errors.load(Ordering::Relaxed),
            dropped_frames: self.inner.stats.dropped_frames.load(Ordering::Relaxed),
            pending_acks: self.inner.ack_waiters.len() as u64,
            queued_messages: self.inner.complete_queue.len() as u64,
            reader_alive: self.inner.stats.reader_alive.load(Ordering::Relaxed),
            reader_errors: self.inner.stats.reader_errors.load(Ordering::Relaxed),
            keepalive_ticks: self.inner.stats.keepalive_ticks.load(Ordering::Relaxed),
            last_keepalive_ns: self.inner.stats.last_keepalive_ns.load(Ordering::Relaxed),
            reassembly_expired: self.inner.stats.reassembly_expired.load(Ordering::Relaxed),
            reassembly_rejected: self.inner.stats.reassembly_rejected.load(Ordering::Relaxed),
            corrupted_rings: transport.corrupted_rings,
            registration_recoveries: transport
                .registration_recoveries
                .saturating_add(transport.registration_request_recoveries)
                .saturating_add(
                    self.inner
                        .stats
                        .registration_recoveries
                        .load(Ordering::Relaxed),
                ),
            blocked_wakeups: transport.blocked_wakeups,
            dead_process_reaps: transport.dead_process_reaps,
            transport_generation: transport.transport_generation,
            switch_recoveries: self.inner.stats.switch_recoveries.load(Ordering::Relaxed),
        }
    }

    pub fn shutdown(&self) {
        self.inner.closing.store(true, Ordering::Release);
        self.inner.keepalive_wait.notify_all();
        self.inner.complete_queue.close();
        self.inner
            .ack_waiters
            .close_all(crate::DATAROUTER_ERR_INTERNAL);
        if let Ok(mut pool) = self.inner.buffer_pool.lock() {
            pool.clear();
        }
    }

    pub fn is_closing(&self) -> bool {
        self.inner.closing.load(Ordering::Acquire)
    }

    #[cfg(debug_assertions)]
    fn track_ipc_request_timing(&self, target_service: &str, packed: &[u8]) -> Option<String> {
        let msg = protocol::unpack(packed).ok()?;
        if msg.message_type != protocol::MSG_BUSINESS_REQUEST {
            return None;
        }
        let fields = protocol::extract_business_fields(&msg);
        if fields.request_id.is_empty() {
            return None;
        }
        let request_id = fields.request_id;
        let timing = IpcTiming {
            target_service: target_service.to_owned(),
            module: fields.module,
            action: fields.action,
            started: Instant::now(),
        };
        if let Ok(mut timings) = self.inner.ipc_timings.lock() {
            timings.insert(request_id.clone(), timing);
        }
        Some(request_id)
    }

    #[cfg(debug_assertions)]
    fn report_ipc_response_timing(&self, msg: &protocol::Message) {
        if msg.message_type != protocol::MSG_BUSINESS_RESPONSE {
            return;
        }
        let fields = protocol::extract_business_fields(msg);
        if fields.request_id.is_empty() {
            return;
        }
        let timing = self
            .inner
            .ipc_timings
            .lock()
            .ok()
            .and_then(|mut timings| timings.remove(&fields.request_id));
        if let Some(timing) = timing {
            println!(
                "[IPC] {}/{}.{} → {}",
                timing.target_service,
                timing.module,
                timing.action,
                format_ipc_elapsed(timing.started.elapsed())
            );
        }
    }

    #[cfg(not(debug_assertions))]
    fn report_ipc_response_timing(&self, _msg: &protocol::Message) {}
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum RecvPackedError {
    Code(c_int),
    BufferTooSmall { required: usize },
}

impl FrontendInner {
    /// Take a receive buffer from the pool (or a fresh empty Vec).
    pub fn take_buffer(&self) -> Vec<u8> {
        self.buffer_pool
            .lock()
            .ok()
            .and_then(|mut pool| pool.pop_front())
            .unwrap_or_default()
    }

    /// Return a drained receive buffer to the pool (bounded to keep pooled
    /// memory at a few channel-sized buffers).
    pub fn recycle_buffer(&self, mut buffer: Vec<u8>) {
        buffer.clear();
        let limit = self.options.app_queue_capacity.min(16).max(1);
        if let Ok(mut pool) = self.buffer_pool.lock()
            && pool.len() < limit
        {
            pool.push_back(buffer);
        }
    }

    pub fn with_port<R>(
        &self,
        f: impl FnOnce(&port_frontend::PortFrontend) -> R,
    ) -> Result<R, c_int> {
        let guard = self
            .port
            .read()
            .map_err(|_| crate::DATAROUTER_ERR_INTERNAL)?;
        let port = guard.as_ref().ok_or(crate::DATAROUTER_ERR_INTERNAL)?;
        Ok(f(port))
    }

    pub fn send_frame(&self, target_service: &str, packed: &[u8]) -> Result<(), c_int> {
        match self.with_port(|port| port.send(target_service, packed))? {
            Ok(()) => Ok(()),
            Err(crate::DATAROUTER_ERR_NOT_FOUND) => {
                self.recover_not_found_and_retry(target_service, packed)
            }
            Err(code) => Err(code),
        }
    }

    pub fn send_frame_to_source(
        &self,
        source: &[c_char; crate::DATAROUTER_SERVICE_NAME_MAX + 1],
        packed: &[u8],
    ) -> Result<(), c_int> {
        self.with_port(|port| port.send_to_c_source(source, packed))?
    }

    /// Direct (unsafe) data-plane send with the same stale-port recovery as
    /// the normal path: a NOT_FOUND whose self registration is gone triggers
    /// a re-register + retry, while a missing target stays NOT_FOUND.
    #[cfg(feature = "unsafe")]
    pub fn unsafe_send_frame(&self, target_service: &str, data: &[u8]) -> Result<(), c_int> {
        match self.with_port(|port| port.unsafe_send(target_service, data))? {
            Ok(()) => Ok(()),
            Err(crate::DATAROUTER_ERR_NOT_FOUND) => {
                self.unsafe_recover_not_found_and_retry(target_service, data)
            }
            Err(code) => Err(code),
        }
    }

    #[cfg(feature = "unsafe")]
    fn unsafe_recover_not_found_and_retry(
        &self,
        target_service: &str,
        data: &[u8],
    ) -> Result<(), c_int> {
        let mut guard = self
            .port
            .write()
            .map_err(|_| crate::DATAROUTER_ERR_INTERNAL)?;
        let recovery = {
            let port = guard.as_ref().ok_or(crate::DATAROUTER_ERR_INTERNAL)?;
            match port.touch() {
                Err(crate::DATAROUTER_ERR_NOT_FOUND) => NotFoundRecovery::ReconnectSelf,
                Err(code) => return Err(code),
                Ok(()) => not_found_recovery(port, &self.options.service_name, target_service)?,
            }
        };
        match recovery {
            NotFoundRecovery::ReconnectSelf => {
                let replacement = port_frontend::PortFrontend::open(&self.options)?;
                let result = replacement.unsafe_send(target_service, data);
                let previous = guard.replace(replacement);
                drop(previous);
                self.stats
                    .registration_recoveries
                    .fetch_add(1, Ordering::Relaxed);
                self.stats.switch_recoveries.fetch_add(1, Ordering::Relaxed);
                result
            }
            NotFoundRecovery::TargetMissing => Err(crate::DATAROUTER_ERR_NOT_FOUND),
            NotFoundRecovery::RetryCurrentPort => guard
                .as_ref()
                .ok_or(crate::DATAROUTER_ERR_INTERNAL)?
                .unsafe_send(target_service, data),
        }
    }

    pub fn touch(&self) -> Result<(), c_int> {
        self.with_port(|port| port.touch())?
    }

    pub(crate) fn recover_self_after_not_found(&self, component: &str) -> bool {
        match self.recover_self_port() {
            Ok(()) => true,
            Err(code) => {
                self.log_recovery_failure(component, code);
                thread::sleep(RECOVERY_RETRY_SLEEP);
                false
            }
        }
    }

    fn recover_self_port(&self) -> Result<(), c_int> {
        let mut guard = self
            .port
            .write()
            .map_err(|_| crate::DATAROUTER_ERR_INTERNAL)?;
        if let Some(port) = guard.as_ref() {
            match port.touch() {
                Ok(()) => return Ok(()),
                Err(crate::DATAROUTER_ERR_NOT_FOUND) => {}
                Err(code) => return Err(code),
            }
        }
        let replacement = port_frontend::PortFrontend::open(&self.options)?;
        *guard = Some(replacement);
        self.stats
            .registration_recoveries
            .fetch_add(1, Ordering::Relaxed);
        self.stats.switch_recoveries.fetch_add(1, Ordering::Relaxed);
        Ok(())
    }

    fn log_recovery_failure(&self, component: &str, code: c_int) {
        self.stats.reader_errors.fetch_add(1, Ordering::Relaxed);
        let now = unix_time_ns();
        let last = self
            .stats
            .last_recovery_error_log_ns
            .load(Ordering::Relaxed);
        if now.saturating_sub(last) < RECOVERY_LOG_INTERVAL_NS {
            return;
        }
        if self
            .stats
            .last_recovery_error_log_ns
            .compare_exchange(last, now, Ordering::Relaxed, Ordering::Relaxed)
            .is_ok()
        {
            eprintln!(
                "datarouter frontend recovery failed component={} service={} shm={} code={}",
                component, self.options.service_name, self.options.shm_name, code
            );
        }
    }

    fn recover_not_found_and_retry(
        &self,
        target_service: &str,
        packed: &[u8],
    ) -> Result<(), c_int> {
        let mut guard = self
            .port
            .write()
            .map_err(|_| crate::DATAROUTER_ERR_INTERNAL)?;
        let recovery = {
            let port = guard.as_ref().ok_or(crate::DATAROUTER_ERR_INTERNAL)?;
            match port.touch() {
                Err(crate::DATAROUTER_ERR_NOT_FOUND) => NotFoundRecovery::ReconnectSelf,
                Err(code) => return Err(code),
                Ok(()) => not_found_recovery(port, &self.options.service_name, target_service)?,
            }
        };
        match recovery {
            NotFoundRecovery::ReconnectSelf => {
                let replacement = port_frontend::PortFrontend::open(&self.options)?;
                let result = replacement.send(target_service, packed);
                let previous = guard.replace(replacement);
                drop(previous);
                self.stats
                    .registration_recoveries
                    .fetch_add(1, Ordering::Relaxed);
                self.stats.switch_recoveries.fetch_add(1, Ordering::Relaxed);
                result
            }
            NotFoundRecovery::TargetMissing => Err(crate::DATAROUTER_ERR_NOT_FOUND),
            NotFoundRecovery::RetryCurrentPort => {
                // Another thread (reader, keepalive, or a concurrent sender)
                // already re-registered this frontend on a fresh port between
                // our failed send and this recovery, or the target registered
                // mid-flight. The port is live and the registry shows both
                // sides, so the original failure was a stale-port artifact:
                // retry the send here instead of surfacing a spurious
                // NOT_FOUND to the application.
                guard
                    .as_ref()
                    .ok_or(crate::DATAROUTER_ERR_INTERNAL)?
                    .send(target_service, packed)
            }
        }
    }
}

impl Drop for Frontend {
    fn drop(&mut self) {
        self.shutdown();
        if let Ok(mut keepalive) = self.keepalive.lock()
            && let Some(handle) = keepalive.take()
        {
            let _ = handle.join();
        }
        if let Ok(mut reader) = self.reader.lock()
            && let Some(handle) = reader.take()
        {
            let _ = handle.join();
        }
    }
}

const RECOVERY_RETRY_SLEEP: Duration = Duration::from_millis(250);
const RECOVERY_LOG_INTERVAL_NS: u64 = 5_000_000_000;

fn spawn_keepalive(inner: Arc<FrontendInner>) -> JoinHandle<()> {
    thread::spawn(move || {
        while !inner.closing.load(Ordering::Acquire) {
            match inner.touch() {
                Ok(()) => {
                    inner.stats.keepalive_ticks.fetch_add(1, Ordering::Relaxed);
                    inner
                        .stats
                        .last_keepalive_ns
                        .store(unix_time_ns(), Ordering::Relaxed);
                }
                Err(code) if inner.closing.load(Ordering::Acquire) => {
                    let _ = code;
                    break;
                }
                Err(crate::DATAROUTER_ERR_NOT_FOUND) => {
                    let _ = inner.recover_self_after_not_found("keepalive");
                }
                Err(code) => {
                    inner.stats.reader_errors.fetch_add(1, Ordering::Relaxed);
                    eprintln!(
                        "datarouter frontend keepalive failed service={} shm={} code={}",
                        inner.options.service_name, inner.options.shm_name, code
                    );
                }
            }
            let Ok(guard) = inner.keepalive_wait_state.lock() else {
                break;
            };
            let _ =
                inner
                    .keepalive_wait
                    .wait_timeout_while(guard, inner.keepalive_interval, |_| {
                        !inner.closing.load(Ordering::Acquire)
                    });
        }
    })
}

fn keepalive_interval(ttl_ms: u32) -> Duration {
    // A quarter of the advertised TTL leaves margin for scheduler jitter and
    // remains strictly below TTL/3 even for sub-4ms test configurations.
    let interval_us = (u64::from(ttl_ms) * 1000 / 4).clamp(1, 1_000_000);
    Duration::from_micros(interval_us)
}

pub(crate) fn unix_time_ns() -> u64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|duration| duration.as_nanos().min(u128::from(u64::MAX)) as u64)
        .unwrap_or(0)
}

pub(crate) fn c_source_to_string(
    source: &[c_char; crate::DATAROUTER_SERVICE_NAME_MAX + 1],
) -> String {
    let bytes: Vec<u8> = source
        .iter()
        .take_while(|value| **value != 0)
        .map(|value| *value as u8)
        .collect();
    String::from_utf8_lossy(&bytes).into_owned()
}

#[derive(Debug, PartialEq, Eq)]
enum NotFoundRecovery {
    ReconnectSelf,
    TargetMissing,
    /// Both this frontend and the target are currently registered while the
    /// port is live: the failed send raced a concurrent self-recovery, so the
    /// send is retried on the existing port instead of reported as missing.
    RetryCurrentPort,
}

fn not_found_recovery(
    port: &port_frontend::PortFrontend,
    self_service: &str,
    target_service: &str,
) -> Result<NotFoundRecovery, c_int> {
    let mut entries = vec![
        crate::datarouter_registry_snapshot_entry {
            service_name: [0 as c_char; crate::DATAROUTER_SERVICE_NAME_MAX + 1],
            pid: 0,
            route_id: 0,
            channel_id: 0,
            slot: 0,
            last_seen_ns: 0,
        };
        crate::DATAROUTER_MAX_PORTS
    ];
    let mut count = 0_usize;
    let rc = port.registry_snapshot(&mut entries, &mut count);
    if rc != crate::DATAROUTER_OK && rc != crate::DATAROUTER_ERR_FULL {
        return Err(rc);
    }
    Ok(not_found_recovery_from_entries(
        entries
            .iter()
            .take(count.min(entries.len()))
            .map(|entry| c_source_to_string(&entry.service_name)),
        self_service,
        target_service,
    ))
}

fn not_found_recovery_from_entries(
    entries: impl IntoIterator<Item = String>,
    self_service: &str,
    target_service: &str,
) -> NotFoundRecovery {
    let mut has_self = false;
    let mut has_target = false;
    for service in entries {
        if service == self_service {
            has_self = true;
        }
        if service == target_service {
            has_target = true;
        }
    }
    match (has_self, has_target) {
        (false, _) => NotFoundRecovery::ReconnectSelf,
        (true, false) => NotFoundRecovery::TargetMissing,
        (true, true) => NotFoundRecovery::RetryCurrentPort,
    }
}

#[cfg(debug_assertions)]
fn format_ipc_elapsed(elapsed: Duration) -> String {
    let elapsed_us = elapsed.as_micros();
    if elapsed_us < 1000 {
        return format!("{elapsed_us} us");
    }
    format!("{:.3} ms", elapsed_us as f64 / 1000.0)
}

#[cfg(test)]
mod tests {
    use super::*;
    #[cfg(unix)]
    use std::process::{Child, Command};
    #[cfg(unix)]
    use std::time::{Duration, SystemTime, UNIX_EPOCH};

    #[test]
    fn not_found_recovery_reconnects_only_when_self_entry_missing() {
        assert_eq!(
            not_found_recovery_from_entries(vec!["core".to_owned()], "portal", "core"),
            NotFoundRecovery::ReconnectSelf
        );
        assert_eq!(
            not_found_recovery_from_entries(vec!["portal".to_owned()], "portal", "core"),
            NotFoundRecovery::TargetMissing
        );
        // Both sides registered on a live port: the failure was a stale-port
        // artifact of a concurrent self-recovery, so the send must be retried
        // on the current port rather than reported as NOT_FOUND.
        assert_eq!(
            not_found_recovery_from_entries(
                vec!["portal".to_owned(), "core".to_owned()],
                "portal",
                "core"
            ),
            NotFoundRecovery::RetryCurrentPort
        );
    }

    #[test]
    fn keepalive_is_bounded_and_strictly_below_one_third_ttl() {
        for ttl_ms in [1_u32, 4, 100, 3_000, 30_000] {
            let interval = keepalive_interval(ttl_ms);
            assert!(interval < Duration::from_micros(u64::from(ttl_ms) * 1000 / 3 + 1));
            assert!(interval <= Duration::from_secs(1));
        }
    }

    #[test]
    fn reader_wait_and_chunk_ttl_reject_unbounded_shutdown_delays() {
        let too_slow_reader = FrontendOptions {
            service_name: "bounded".to_owned(),
            reader_wait_ms: options::MAX_READER_WAIT_MS + 1,
            ..FrontendOptions::default()
        };
        assert!(matches!(
            too_slow_reader.normalized(),
            Err(crate::DATAROUTER_ERR_INVALID_ARGUMENT)
        ));
        let too_long_ttl = FrontendOptions {
            service_name: "bounded".to_owned(),
            chunk_ttl_ms: options::MAX_CHUNK_TTL_MS + 1,
            ..FrontendOptions::default()
        };
        assert!(matches!(
            too_long_ttl.normalized(),
            Err(crate::DATAROUTER_ERR_INVALID_ARGUMENT)
        ));
    }

    #[cfg(unix)]
    #[test]
    fn concurrent_sends_never_report_spurious_not_found_after_slot_reclaim() {
        let Some(mut switch) = start_test_switch("rust-stress-race") else {
            return;
        };
        let shm_name = switch.shm_name.clone();
        let _core = Frontend::connect(FrontendOptions {
            shm_name: shm_name.clone(),
            service_name: "core".to_owned(),
            connect_timeout_ms: 1000,
            recv_timeout_ms: 1000,
            ..FrontendOptions::default()
        })
        .expect("connect core frontend");
        let local = Frontend::connect(FrontendOptions {
            shm_name,
            service_name: "portal".to_owned(),
            connect_timeout_ms: 1000,
            recv_timeout_ms: 1000,
            ..FrontendOptions::default()
        })
        .expect("connect local frontend");

        // Reclaim the portal slot each round, let a fresh occupant steal it,
        // then fire concurrent sends. One sender (or the reader/keepalive
        // threads) re-registers portal; every other sender must retry on the
        // fresh port instead of surfacing a spurious NOT_FOUND from the stale
        // slot it failed on.
        let mut failures = 0usize;
        for round in 0..25 {
            local
                .inner
                .with_port(|port| port.test_release_self_slot())
                .expect("access local port")
                .expect("release local slot");
            let occupant = Frontend::connect(FrontendOptions {
                shm_name: switch.shm_name.clone(),
                service_name: "occupant".to_owned(),
                connect_timeout_ms: 1000,
                recv_timeout_ms: 1000,
                ..FrontendOptions::default()
            })
            .expect("connect slot occupant");

            let mut msg = protocol::Message::new(protocol::MSG_BUSINESS_REQUEST);
            msg.body = format!("race-{round}").into_bytes();
            let packed = protocol::pack(&msg).expect("pack message");
            let mut handles = Vec::new();
            for _ in 0..4 {
                let inner = std::sync::Arc::clone(&local.inner);
                let packed = packed.clone();
                handles.push(std::thread::spawn(move || {
                    std::thread::sleep(Duration::from_millis(1));
                    super::outbound::send(&inner, "core", &packed, 1000)
                }));
            }
            let results: Vec<Result<(), i32>> = handles
                .into_iter()
                .map(|handle| handle.join().expect("sender thread"))
                .collect();
            for result in results {
                if let Err(crate::DATAROUTER_ERR_NOT_FOUND) = result {
                    failures += 1;
                } else if let Err(code) = result {
                    panic!("unexpected send error round={round} code={code}");
                }
            }
            drop(occupant);
        }
        assert_eq!(failures, 0, "spurious NOT_FOUND on {failures} sends");
        switch.stop();
    }

    #[cfg(unix)]
    #[test]
    fn send_recovers_when_self_slot_was_reclaimed() {
        let Some(mut switch) = start_test_switch("rust-self-heal") else {
            return;
        };
        let shm_name = switch.shm_name.clone();
        let core = Frontend::connect(FrontendOptions {
            shm_name: shm_name.clone(),
            service_name: "core".to_owned(),
            connect_timeout_ms: 1000,
            recv_timeout_ms: 1000,
            ..FrontendOptions::default()
        })
        .expect("connect core frontend");
        let local = Frontend::connect(FrontendOptions {
            shm_name,
            service_name: "portal".to_owned(),
            connect_timeout_ms: 1000,
            recv_timeout_ms: 1000,
            ..FrontendOptions::default()
        })
        .expect("connect local frontend");

        local
            .inner
            .with_port(|port| port.test_release_self_slot())
            .expect("access local port")
            .expect("release local slot");
        let _occupant = Frontend::connect(FrontendOptions {
            shm_name: switch.shm_name.clone(),
            service_name: "occupant".to_owned(),
            connect_timeout_ms: 1000,
            recv_timeout_ms: 1000,
            ..FrontendOptions::default()
        })
        .expect("connect slot occupant");

        let mut msg = protocol::Message::new(protocol::MSG_BUSINESS_REQUEST);
        msg.body = b"self-healed".to_vec();
        let packed = protocol::pack(&msg).expect("pack message");
        local
            .send("core", &packed, 1000)
            .expect("send retries after self slot recovery");

        let received = core.recv(1000).expect("core receives retried message");
        assert_eq!(received.source, "portal");
        assert_eq!(received.message.body, b"self-healed");

        switch.stop();
    }

    #[cfg(unix)]
    #[test]
    fn reader_recovers_when_self_slot_was_reclaimed() {
        let Some(mut switch) = start_test_switch("rust-reader-self-heal") else {
            return;
        };
        let shm_name = switch.shm_name.clone();
        let core = Frontend::connect(FrontendOptions {
            shm_name: shm_name.clone(),
            service_name: "core".to_owned(),
            connect_timeout_ms: 1000,
            recv_timeout_ms: 1000,
            ..FrontendOptions::default()
        })
        .expect("connect core frontend");
        let local = Frontend::connect(FrontendOptions {
            shm_name,
            service_name: "portal".to_owned(),
            connect_timeout_ms: 1000,
            recv_timeout_ms: 1000,
            reader_wait_ms: 10,
            ..FrontendOptions::default()
        })
        .expect("connect local frontend");

        local
            .inner
            .with_port(|port| port.test_release_self_slot())
            .expect("access local port")
            .expect("release local slot");
        let _occupant = Frontend::connect(FrontendOptions {
            shm_name: switch.shm_name.clone(),
            service_name: "occupant".to_owned(),
            connect_timeout_ms: 1000,
            recv_timeout_ms: 1000,
            ..FrontendOptions::default()
        })
        .expect("connect slot occupant");

        let mut msg = protocol::Message::new(protocol::MSG_BUSINESS_REQUEST);
        msg.body = b"reader-self-healed".to_vec();
        let packed = protocol::pack(&msg).expect("pack message");
        let mut sent = false;
        for _ in 0..100 {
            match core.send("portal", &packed, 1000) {
                Ok(()) => {
                    sent = true;
                    break;
                }
                Err(crate::DATAROUTER_ERR_NOT_FOUND) => {
                    std::thread::sleep(Duration::from_millis(10));
                }
                Err(code) => panic!("unexpected send error: {code}"),
            }
        }
        assert!(sent, "core sends after reader self recovery");

        let received = local
            .recv(1000)
            .expect("local receives after self recovery");
        assert_eq!(received.source, "core");
        assert_eq!(received.message.body, b"reader-self-healed");

        switch.stop();
    }

    #[cfg(unix)]
    #[test]
    fn existing_frontends_recover_after_switch_crash_and_stale_rebuild() {
        let Some(mut switch) = start_test_switch("rust-switch-restart") else {
            return;
        };
        let options = |service_name: &str| FrontendOptions {
            shm_name: switch.shm_name.clone(),
            service_name: service_name.to_owned(),
            connect_timeout_ms: 1000,
            recv_timeout_ms: 1000,
            reader_wait_ms: 10,
            ..FrontendOptions::default()
        };
        let core = Frontend::connect(options("core")).expect("connect core frontend");
        let local = Frontend::connect(options("portal")).expect("connect portal frontend");

        switch.restart();

        let mut msg = protocol::Message::new(protocol::MSG_BUSINESS_REQUEST);
        msg.body = b"after-switch-restart".to_vec();
        let packed = protocol::pack(&msg).expect("pack restart message");
        let mut sent = false;
        for _ in 0..300 {
            match local.send("core", &packed, 250) {
                Ok(()) => {
                    sent = true;
                    break;
                }
                Err(crate::DATAROUTER_ERR_NOT_FOUND)
                | Err(crate::DATAROUTER_ERR_TIMEOUT)
                | Err(crate::DATAROUTER_ERR_BUSY) => {
                    std::thread::sleep(Duration::from_millis(10));
                }
                Err(code) => panic!("unexpected restart recovery error: {code}"),
            }
        }
        assert!(
            sent,
            "original portal handle re-registers after switch restart"
        );
        let received = core
            .recv(1000)
            .expect("original core handle receives after restart");
        assert_eq!(received.source, "portal");
        assert_eq!(received.message.body, b"after-switch-restart");

        switch.stop();
    }

    #[cfg(unix)]
    struct TestSwitch {
        child: Option<Child>,
        shm_name: String,
    }

    #[cfg(unix)]
    impl TestSwitch {
        fn spawn(shm_name: &str) -> Child {
            Command::new("build/datarouter-switch")
                .arg("--name")
                .arg(shm_name)
                .arg("--ttl-ms")
                .arg("5000")
                .arg("--allow")
                .arg("core,portal,occupant")
                .spawn()
                .expect("start datarouter-switch")
        }

        fn restart(&mut self) {
            if let Some(mut child) = self.child.take() {
                let _ = child.kill();
                let _ = child.wait();
            }
            self.child = Some(Self::spawn(&self.shm_name));
            std::thread::sleep(Duration::from_millis(100));
        }

        fn stop(&mut self) {
            if let Some(mut child) = self.child.take() {
                let _ = child.kill();
                let _ = child.wait();
            }
            unlink_shm(&self.shm_name);
        }
    }

    #[cfg(unix)]
    impl Drop for TestSwitch {
        fn drop(&mut self) {
            self.stop();
        }
    }

    #[cfg(unix)]
    fn start_test_switch(prefix: &str) -> Option<TestSwitch> {
        let switch_path = std::path::Path::new("build/datarouter-switch");
        if !switch_path.exists() {
            eprintln!("skipping self-heal test: build/datarouter-switch is not built");
            return None;
        }
        let nanos = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .expect("system time")
            .as_nanos();
        let shm_name = format!("/datarouter-{prefix}-{nanos}");
        unlink_shm(&shm_name);
        let child = TestSwitch::spawn(&shm_name);
        std::thread::sleep(Duration::from_millis(50));
        Some(TestSwitch {
            child: Some(child),
            shm_name,
        })
    }

    #[cfg(unix)]
    fn unlink_shm(name: &str) {
        let _ = std::fs::remove_file(format!("/dev/shm/{}", name.trim_start_matches('/')));
    }
}
