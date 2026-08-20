//! Datarouter IPC protocol codec.
//!
//! This module implements the binary message format used for inter-process
//! communication over the datarouter shared-memory transport. It handles
//! all framing (8-byte header, TLV var_data, chunking, ack flow control)
//! so that applications only deal with business-level concepts.
//!
//! ## Wire Format
//!
//! ```text
//! Offset  Size  Field
//! 0       1     message_type
//! 1       1     flags
//! 2       2     var_data_length (LE)
//! 4       4     body_length (LE)
//! 8       N     var_data (TLV-encoded)
//! 8+N     M     body (raw bytes)
//! ```
//!
//! ## Message Types
//!
//! | Direction     | Name                   | Value | Description                |
//! |---------------|------------------------|-------|----------------------------|
//! | Edge -> Core  | MSG_BUSINESS_REQUEST   | 0x07  | Business request           |
//! | Core -> Edge  | MSG_BUSINESS_RESPONSE  | 0x8C  | Business response          |
//! | Either        | MSG_CHUNK_ACK          | 0x05  | Chunk acknowledgment       |
//! | Either        | MSG_CHUNK_ERROR        | 0x06  | Chunk error                |
//! | Core -> Edge  | MSG_ROUTES_REFRESH     | 0x85  | Route registration         |
//! | Core -> Edge  | MSG_CACHE_INVALIDATE   | 0x87  | Cache invalidation         |
//! | Core -> Edge  | MSG_AGENT_REGISTRY     | 0x88  | Agent registry update      |
//! | Core -> Edge  | MSG_AGENT_REQUEST      | 0x89  | Agent task request         |
//! | Core -> Edge  | MSG_AGENT_EVENT        | 0x8A  | Agent event                |
//! | Core -> Edge  | MSG_CONTROL            | 0x8B  | Control message            |
//! | Core -> Watcher | MSG_LOG_NOTICE       | 0x8D  | Raw log notice             |

use std::collections::HashMap;
use std::hash::{BuildHasher, Hasher};
use std::sync::atomic::{AtomicU32, AtomicU64, AtomicUsize, Ordering};
use std::sync::{Arc, Mutex, OnceLock};
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

// ── Message Types ──────────────────────────────────────────────────────────

/// Business request from edge to core.
pub const MSG_BUSINESS_REQUEST: u8 = 0x07;
/// Business response from core to edge.
pub const MSG_BUSINESS_RESPONSE: u8 = 0x8C;
/// Chunk acknowledgment.
pub const MSG_CHUNK_ACK: u8 = 0x05;
/// Chunk error.
pub const MSG_CHUNK_ERROR: u8 = 0x06;
/// Route registration from core to edge.
pub const MSG_ROUTES_REFRESH: u8 = 0x85;
/// Cache invalidation from core to edge.
pub const MSG_CACHE_INVALIDATE: u8 = 0x87;
/// Agent registry update from core to edge.
pub const MSG_AGENT_REGISTRY_UPDATE: u8 = 0x88;
/// Agent task request from core to edge.
pub const MSG_AGENT_REQUEST: u8 = 0x89;
/// Agent event from core to edge.
pub const MSG_AGENT_EVENT: u8 = 0x8A;
/// Control message from core to edge.
pub const MSG_CONTROL: u8 = 0x8B;
/// Raw log notice reserved for future watcher delivery.
pub const MSG_LOG_NOTICE: u8 = 0x8D;

// ── TLV Tags ──────────────────────────────────────────────────────────────

/// Connection/session identifier string.
pub const TAG_CONNECTION_ID: u8 = 0x04;
/// Request-response correlation string.
pub const TAG_REQUEST_ID: u8 = 0x05;
/// Status code (u16 LE).
pub const TAG_STATUS: u8 = 0x06;
/// Text flag (byte: 1=text, 0=binary).
pub const TAG_IS_TEXT: u8 = 0x07;
/// Chunk group identifier (u64 LE).
pub const TAG_CHUNK_ID: u8 = 0x08;
/// Chunk index (u16 LE).
pub const TAG_CHUNK_INDEX: u8 = 0x09;
/// Total chunk count (u16 LE).
pub const TAG_CHUNK_COUNT: u8 = 0x0A;
/// Request context JSON.
pub const TAG_CONTEXT: u8 = 0x0D;
/// JSON data payload.
pub const TAG_JSON_DATA: u8 = 0x0E;
/// Module name string (e.g. "admin", "agent", "public").
pub const TAG_MODULE: u8 = 0x0F;
/// Action string (e.g. "auth.login", "visitors.live.subscribe").
pub const TAG_ACTION: u8 = 0x10;
/// Streaming flag (byte: 1=expect multiple responses).
pub const TAG_STREAMING: u8 = 0x11;
/// Workflow identifier string.
pub const TAG_WORKFLOW_ID: u8 = 0x12;
/// Task UUID string.
pub const TAG_TASK_UUID: u8 = 0x13;
/// Step identifier string.
pub const TAG_STEP_ID: u8 = 0x14;
/// Actor UUID string.
pub const TAG_ACTOR_UUID: u8 = 0x15;
/// Trace identifier string (distributed tracing correlation).
pub const TAG_TRACE_ID: u8 = 0x16;

/// Maximum application payload size. The full 512 KiB ring is usable as one
/// single frame; the legacy 60 KiB cap is disabled.
pub const MAX_APP_PAYLOAD: usize = crate::DATAROUTER_MAX_APP_PAYLOAD;
/// Header size in bytes.
pub const HEADER_SIZE: usize = 8;

static CHUNK_ID_PREFIX: OnceLock<u32> = OnceLock::new();
static NEXT_CHUNK_ID: AtomicU32 = AtomicU32::new(1);

/// Bounded, size-classed pool for reusable non-sensitive wire buffers.
#[derive(Clone)]
pub struct WireBufferPool {
    inner: Arc<WireBufferPoolInner>,
}

struct WireBufferPoolInner {
    classes: [usize; 3],
    buckets: [Mutex<Vec<Vec<u8>>>; 3],
    max_retained_bytes: usize,
    retained_bytes: AtomicUsize,
    reuses: AtomicU64,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct WireBufferPoolStats {
    pub retained_bytes: usize,
    pub reuses: u64,
}

impl WireBufferPool {
    pub fn new(mut classes: [usize; 3], max_retained_bytes: usize) -> Result<Self, &'static str> {
        classes.sort_unstable();
        if classes[0] == 0
            || classes[1] == classes[0]
            || classes[2] == classes[1]
            || max_retained_bytes < classes[0]
        {
            return Err("invalid wire buffer pool limits");
        }
        Ok(Self {
            inner: Arc::new(WireBufferPoolInner {
                classes,
                buckets: std::array::from_fn(|_| Mutex::new(Vec::new())),
                max_retained_bytes,
                retained_bytes: AtomicUsize::new(0),
                reuses: AtomicU64::new(0),
            }),
        })
    }

    pub fn acquire(&self, minimum_capacity: usize) -> Vec<u8> {
        if let Some(index) = self
            .inner
            .classes
            .iter()
            .position(|class| *class >= minimum_capacity)
            && let Ok(mut bucket) = self.inner.buckets[index].lock()
            && let Some(mut buffer) = bucket.pop()
        {
            self.inner
                .retained_bytes
                .fetch_sub(buffer.capacity(), Ordering::Relaxed);
            self.inner.reuses.fetch_add(1, Ordering::Relaxed);
            buffer.clear();
            return buffer;
        }
        let capacity = self
            .inner
            .classes
            .iter()
            .copied()
            .find(|class| *class >= minimum_capacity)
            .unwrap_or(minimum_capacity);
        Vec::with_capacity(capacity)
    }

    pub fn release(&self, mut buffer: Vec<u8>) {
        buffer.clear();
        let capacity = buffer.capacity();
        let Some(index) = self
            .inner
            .classes
            .iter()
            .position(|class| capacity == *class)
        else {
            return;
        };
        if self
            .inner
            .retained_bytes
            .fetch_update(Ordering::AcqRel, Ordering::Relaxed, |current| {
                current
                    .checked_add(capacity)
                    .filter(|next| *next <= self.inner.max_retained_bytes)
            })
            .is_err()
        {
            return;
        }
        if let Ok(mut bucket) = self.inner.buckets[index].lock() {
            bucket.push(buffer);
        } else {
            self.inner
                .retained_bytes
                .fetch_sub(capacity, Ordering::Relaxed);
        }
    }

    pub fn stats(&self) -> WireBufferPoolStats {
        WireBufferPoolStats {
            retained_bytes: self.inner.retained_bytes.load(Ordering::Relaxed),
            reuses: self.inner.reuses.load(Ordering::Relaxed),
        }
    }
}

// ── Message ───────────────────────────────────────────────────────────────

/// A decoded datarouter protocol message.
#[derive(Debug, Clone, PartialEq)]
pub struct Message {
    pub message_type: u8,
    pub flags: u8,
    pub var_data: Vec<u8>,
    pub body: Vec<u8>,
}

impl Message {
    /// Create a new message with the given type.
    pub fn new(message_type: u8) -> Self {
        Self {
            message_type,
            flags: 0,
            var_data: Vec::new(),
            body: Vec::new(),
        }
    }

    /// Return a human-readable name for the message type.
    pub fn type_name(&self) -> &'static str {
        type_name(self.message_type)
    }
}

/// Return a human-readable name for a message type constant.
pub fn type_name(t: u8) -> &'static str {
    match t {
        MSG_BUSINESS_REQUEST => "business.request",
        MSG_BUSINESS_RESPONSE => "business.response",
        MSG_CHUNK_ACK => "chunk.ack",
        MSG_CHUNK_ERROR => "chunk.error",
        MSG_ROUTES_REFRESH => "routes.refresh",
        MSG_CACHE_INVALIDATE => "cache.invalidate",
        MSG_AGENT_REGISTRY_UPDATE => "agent.registry.update",
        MSG_AGENT_REQUEST => "agent.request",
        MSG_AGENT_EVENT => "agent.event",
        MSG_CONTROL => "control",
        MSG_LOG_NOTICE => "log.notice",
        _ => "unknown",
    }
}

// ── Wire Format ───────────────────────────────────────────────────────────

/// Serialize a Message into the binary wire format.
pub fn pack(msg: &Message) -> Result<Vec<u8>, &'static str> {
    let mut out = Vec::new();
    pack_into(msg, &mut out)?;
    Ok(out)
}

/// Serialize a message into a caller-owned buffer.
///
/// The buffer is cleared but its allocation is retained. This is the preferred
/// API for hot response paths and buffer pools; [`pack`] remains as a
/// compatibility wrapper.
pub fn pack_into(msg: &Message, out: &mut Vec<u8>) -> Result<(), &'static str> {
    let vd_len = msg.var_data.len();
    let b_len = msg.body.len();
    let vd_len_wire = u16::try_from(vd_len).map_err(|_| "var_data exceeds u16")?;
    let b_len_wire = u32::try_from(b_len).map_err(|_| "body exceeds u32")?;
    validate_tlvs(&msg.var_data)?;
    let total_len = HEADER_SIZE
        .checked_add(vd_len)
        .and_then(|len| len.checked_add(b_len))
        .ok_or("message length overflow")?;
    out.clear();
    out.try_reserve(total_len)
        .map_err(|_| "message buffer allocation failed")?;
    out.push(msg.message_type);
    out.push(msg.flags);
    out.extend_from_slice(&vd_len_wire.to_le_bytes());
    out.extend_from_slice(&b_len_wire.to_le_bytes());
    out.extend_from_slice(&msg.var_data);
    out.extend_from_slice(&msg.body);
    Ok(())
}

/// Deserialize a Message from the binary wire format.
pub fn unpack(data: &[u8]) -> Result<Message, &'static str> {
    if data.len() < HEADER_SIZE {
        return Err("frame too short");
    }
    let (message_type, flags, vd_len, b_len) =
        raw_frame_layout(data).ok_or("frame has trailing data")?;
    validate_tlvs(&data[HEADER_SIZE..HEADER_SIZE + vd_len])?;
    Ok(Message {
        message_type,
        flags,
        var_data: data[HEADER_SIZE..HEADER_SIZE + vd_len].to_vec(),
        body: data[HEADER_SIZE + vd_len..HEADER_SIZE + vd_len + b_len].to_vec(),
    })
}

// ── TLV Helpers ───────────────────────────────────────────────────────────

/// Read a raw byte slice field from var_data by tag.
pub fn tlv_read_bytes(var_data: &[u8], tag: u8) -> Option<&[u8]> {
    if validate_tlvs(var_data).is_err() {
        return None;
    }
    let mut i = 0;
    while i + 3 <= var_data.len() {
        let t = var_data[i];
        let l = u16::from_le_bytes([var_data[i + 1], var_data[i + 2]]) as usize;
        i += 3;
        if i + l > var_data.len() {
            return None;
        }
        if t == tag {
            return Some(&var_data[i..i + l]);
        }
        i += l;
    }
    None
}

/// Read a UTF-8 string field from var_data by tag.
pub fn tlv_read_string(var_data: &[u8], tag: u8) -> Option<String> {
    tlv_read_bytes(var_data, tag)
        .and_then(|b| std::str::from_utf8(b).ok())
        .map(str::to_owned)
}

/// Read a u16 LE field from var_data by tag.
pub fn tlv_read_u16(var_data: &[u8], tag: u8) -> Option<u16> {
    tlv_read_bytes(var_data, tag).and_then(|b| {
        if b.len() == 2 {
            Some(u16::from_le_bytes([b[0], b[1]]))
        } else {
            None
        }
    })
}

/// Read a single byte field from var_data by tag.
pub fn tlv_read_byte(var_data: &[u8], tag: u8) -> Option<u8> {
    tlv_read_bytes(var_data, tag).and_then(|b| (b.len() == 1).then(|| b[0]))
}

/// Read all TLV entries from var_data as a map of tag -> bytes.
pub fn tlv_read_all(var_data: &[u8]) -> HashMap<u8, Vec<u8>> {
    let mut map = HashMap::new();
    if validate_tlvs(var_data).is_err() {
        return map;
    }
    let mut i = 0;
    while i + 3 <= var_data.len() {
        let t = var_data[i];
        let l = u16::from_le_bytes([var_data[i + 1], var_data[i + 2]]) as usize;
        i += 3;
        if i + l > var_data.len() {
            break;
        }
        map.insert(t, var_data[i..i + l].to_vec());
        i += l;
    }
    map
}

/// Validate that a TLV block contains only complete, exactly-sized entries.
///
/// Delegates to the C single source of truth (`src/validation.c`) via FFI.
pub fn validate_tlvs(var_data: &[u8]) -> Result<(), &'static str> {
    if crate::validation::validate_tlvs(var_data) {
        Ok(())
    } else {
        Err("malformed TLV")
    }
}

// ── VarData Builder ───────────────────────────────────────────────────────

/// Builder for constructing TLV-encoded var_data blocks.
pub struct VarDataBuilder {
    buf: Vec<u8>,
    error: Option<&'static str>,
}

impl Default for VarDataBuilder {
    fn default() -> Self {
        Self::new()
    }
}

impl VarDataBuilder {
    pub fn new() -> Self {
        Self {
            buf: Vec::new(),
            error: None,
        }
    }

    fn put(&mut self, tag: u8, data: &[u8]) -> &mut Self {
        let Ok(len) = u16::try_from(data.len()) else {
            self.error = Some("TLV value exceeds u16");
            return self;
        };
        if self
            .buf
            .len()
            .checked_add(3)
            .and_then(|value| value.checked_add(data.len()))
            .is_none()
        {
            self.error = Some("TLV builder length overflow");
            return self;
        }
        self.buf.push(tag);
        self.buf.extend_from_slice(&len.to_le_bytes());
        self.buf.extend_from_slice(data);
        self
    }

    pub fn put_string(&mut self, tag: u8, s: &str) -> &mut Self {
        if !s.is_empty() {
            self.put(tag, s.as_bytes());
        }
        self
    }

    pub fn put_u16(&mut self, tag: u8, v: u16) -> &mut Self {
        self.put(tag, &v.to_le_bytes())
    }

    pub fn put_byte(&mut self, tag: u8, v: u8) -> &mut Self {
        self.put(tag, &[v])
    }

    pub fn put_bytes(&mut self, tag: u8, data: &[u8]) -> &mut Self {
        if !data.is_empty() {
            self.put(tag, data);
        }
        self
    }

    pub fn module(&mut self, s: &str) -> &mut Self {
        self.put_string(TAG_MODULE, s)
    }
    pub fn action(&mut self, s: &str) -> &mut Self {
        self.put_string(TAG_ACTION, s)
    }
    pub fn request_id(&mut self, s: &str) -> &mut Self {
        self.put_string(TAG_REQUEST_ID, s)
    }
    pub fn connection_id(&mut self, s: &str) -> &mut Self {
        self.put_string(TAG_CONNECTION_ID, s)
    }
    pub fn workflow_id(&mut self, s: &str) -> &mut Self {
        self.put_string(TAG_WORKFLOW_ID, s)
    }
    pub fn task_uuid(&mut self, s: &str) -> &mut Self {
        self.put_string(TAG_TASK_UUID, s)
    }
    pub fn step_id(&mut self, s: &str) -> &mut Self {
        self.put_string(TAG_STEP_ID, s)
    }
    pub fn actor_uuid(&mut self, s: &str) -> &mut Self {
        self.put_string(TAG_ACTOR_UUID, s)
    }
    pub fn trace_id(&mut self, s: &str) -> &mut Self {
        self.put_string(TAG_TRACE_ID, s)
    }

    pub fn status(&mut self, code: u16) -> &mut Self {
        self.put_u16(TAG_STATUS, code)
    }

    pub fn is_text(&mut self, v: bool) -> &mut Self {
        self.put_byte(TAG_IS_TEXT, if v { 1 } else { 0 })
    }

    pub fn streaming(&mut self, v: bool) -> &mut Self {
        self.put_byte(TAG_STREAMING, if v { 1 } else { 0 })
    }

    pub fn chunk_id(&mut self, id: u64) -> &mut Self {
        self.put_bytes(TAG_CHUNK_ID, &id.to_le_bytes())
    }

    pub fn chunk_index(&mut self, i: u16) -> &mut Self {
        self.put_u16(TAG_CHUNK_INDEX, i)
    }
    pub fn chunk_count(&mut self, n: u16) -> &mut Self {
        self.put_u16(TAG_CHUNK_COUNT, n)
    }

    pub fn context(&mut self, json: &[u8]) -> &mut Self {
        self.put_bytes(TAG_CONTEXT, json)
    }
    pub fn json_data(&mut self, json: &[u8]) -> &mut Self {
        self.put_bytes(TAG_JSON_DATA, json)
    }

    pub fn build(&mut self) -> Vec<u8> {
        if self.error.take().is_some() {
            self.buf.clear();
            // Preserve the infallible compatibility API while ensuring the
            // resulting block is rejected by `pack` instead of truncating.
            return vec![0, 1, 0];
        }
        std::mem::take(&mut self.buf)
    }

    /// Finish this builder and report an oversized field to Rust callers.
    pub fn try_build(&mut self) -> Result<Vec<u8>, &'static str> {
        if let Some(error) = self.error.take() {
            self.buf.clear();
            return Err(error);
        }
        Ok(std::mem::take(&mut self.buf))
    }
}

// ── Chunking ──────────────────────────────────────────────────────────────

/// A single chunk of a larger message.
pub struct ChunkedMessage {
    pub message: Message,
    pub chunk_id: u64,
    pub chunk_index: u16,
    pub chunk_count: u16,
}

/// Split a message into chunks that fit within `max_payload`.
pub fn chunk(msg: &Message, max_payload: usize) -> Result<Vec<ChunkedMessage>, &'static str> {
    if max_payload <= HEADER_SIZE + 64 {
        return Err("max payload too small");
    }
    validate_tlvs(&msg.var_data)?;

    let chunk_vd_overhead: usize = 8 + 3 + 2 + 3 + 2 + 3; // 21 bytes
    let chunk_var_data_len = msg
        .var_data
        .len()
        .checked_add(chunk_vd_overhead)
        .ok_or("chunk metadata length overflow")?;
    u16::try_from(chunk_var_data_len).map_err(|_| "chunk var_data exceeds u16")?;
    let fixed_size = HEADER_SIZE
        .checked_add(chunk_var_data_len)
        .ok_or("chunk fixed length overflow")?;
    let body_budget = max_payload
        .checked_sub(fixed_size)
        .ok_or("chunk metadata exceeds max payload")?;
    if !msg.body.is_empty() && body_budget == 0 {
        return Err("no room for chunk body");
    }

    let chunk_count = if msg.body.is_empty() {
        1
    } else {
        msg.body.len().div_ceil(body_budget).max(1)
    };
    if chunk_count > 0xFFFF {
        return Err("too many chunks");
    }

    let sequence = NEXT_CHUNK_ID.fetch_add(1, Ordering::Relaxed);
    let base_prefix = *CHUNK_ID_PREFIX.get_or_init(chunk_id_prefix);
    let prefix = chunk_id_prefix_for_pid(base_prefix, std::process::id());
    let id = (u64::from(prefix) << 32) | u64::from(sequence);
    let mut chunks = Vec::with_capacity(chunk_count);

    for i in 0..chunk_count {
        let start = i * body_budget;
        let end = ((i + 1) * body_budget).min(msg.body.len());
        let chunk_body = if msg.body.is_empty() {
            Vec::new()
        } else {
            msg.body[start..end].to_vec()
        };

        let mut vd = VarDataBuilder::new();
        vd.chunk_id(id)
            .chunk_index(i as u16)
            .chunk_count(chunk_count as u16);
        let mut vd_bytes = vd.build();
        vd_bytes.extend_from_slice(&msg.var_data);

        chunks.push(ChunkedMessage {
            message: Message {
                message_type: msg.message_type,
                flags: msg.flags,
                var_data: vd_bytes,
                body: chunk_body,
            },
            chunk_id: id,
            chunk_index: i as u16,
            chunk_count: chunk_count as u16,
        });
    }
    Ok(chunks)
}

fn chunk_id_prefix() -> u32 {
    // `RandomState` is independently keyed for each process. Mix in process
    // and clock state as a fallback against platforms with weak startup
    // entropy, and reserve zero as an invalid-looking prefix.
    let random = std::collections::hash_map::RandomState::new();
    let mut hasher = random.build_hasher();
    hasher.write_u32(std::process::id());
    hasher.write_u128(
        SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .unwrap_or_default()
            .as_nanos(),
    );
    hasher.write_usize((&random as *const _) as usize);
    let prefix = hasher.finish() as u32;
    if prefix == 0 { 1 } else { prefix }
}

fn chunk_id_prefix_for_pid(base_prefix: u32, pid: u32) -> u32 {
    let mixed = base_prefix
        .wrapping_add(pid.rotate_left(13))
        .wrapping_mul(0x9e37_79b1)
        ^ pid.wrapping_mul(0x85eb_ca6b);
    if mixed == 0 { 1 } else { mixed }
}

// ── Assembler ─────────────────────────────────────────────────────────────

#[derive(Debug, Clone, Copy)]
pub struct ReassemblyLimits {
    pub ttl: Duration,
    pub max_groups: usize,
    pub max_bytes: usize,
    pub max_message_bytes: usize,
}

impl Default for ReassemblyLimits {
    fn default() -> Self {
        Self {
            ttl: Duration::from_secs(30),
            max_groups: 1024,
            max_bytes: 128 * 1024 * 1024,
            max_message_bytes: 64 * 1024 * 1024,
        }
    }
}

#[derive(Debug)]
struct AssemblyGroup {
    message_type: u8,
    flags: u8,
    common_var_data: Vec<u8>,
    chunks: Vec<Option<Vec<u8>>>,
    payload_bytes: usize,
    memory_bytes: usize,
    last_chunk_index: u16,
    final_ack: Message,
    last_updated: Instant,
}

#[derive(Debug)]
struct ChunkMetadata {
    id: u64,
    index: u16,
    count: u16,
    common_var_data: Vec<u8>,
}

#[derive(Debug)]
pub enum AssemblyOutcome {
    Pending {
        ack: Option<Message>,
    },
    Complete {
        message: Message,
        ack: Option<Message>,
        final_ack: Option<Message>,
    },
    Rejected {
        error: Option<Message>,
    },
}

#[derive(Debug)]
pub struct ExpiredAssembly {
    pub source: String,
    pub error: Message,
}

/// Reassembles chunked messages while bounding all retained state.
///
/// Groups are keyed by both source service and chunk id. This is important:
/// chunk ids are only unique within a sender process and are not an identity
/// credential for another service.
pub struct ChunkAssembler {
    groups: HashMap<(String, u64), AssemblyGroup>,
    retained_bytes: usize,
    limits: ReassemblyLimits,
}

impl Default for ChunkAssembler {
    fn default() -> Self {
        Self::new()
    }
}

impl ChunkAssembler {
    pub fn new() -> Self {
        Self::with_limits(ReassemblyLimits::default())
    }

    pub fn with_limits(limits: ReassemblyLimits) -> Self {
        Self {
            groups: HashMap::new(),
            retained_bytes: 0,
            limits,
        }
    }

    /// Compatibility helper for callers that do not need source isolation.
    pub fn add(&mut self, msg: Message) -> Option<Message> {
        match self.add_from("", msg) {
            AssemblyOutcome::Complete { message, .. } => Some(message),
            AssemblyOutcome::Pending { .. } | AssemblyOutcome::Rejected { .. } => None,
        }
    }

    pub fn add_from(&mut self, source: &str, msg: Message) -> AssemblyOutcome {
        let metadata = match parse_chunk_metadata(&msg.var_data) {
            Ok(None) => {
                return AssemblyOutcome::Complete {
                    message: msg,
                    ack: None,
                    final_ack: None,
                };
            }
            Ok(Some(metadata)) => metadata,
            Err(_) => {
                let control_fields = chunk_control_fields(&msg);
                if let Some((id, _)) = control_fields {
                    self.remove_group(&(source.to_owned(), id));
                }
                return AssemblyOutcome::Rejected {
                    error: control_fields
                        .map(|(id, index)| chunk_control_message(MSG_CHUNK_ERROR, id, index)),
                };
            }
        };
        let error = Some(chunk_control_message(
            MSG_CHUNK_ERROR,
            metadata.id,
            metadata.index,
        ));
        if metadata.count == 0 || metadata.index >= metadata.count {
            return AssemblyOutcome::Rejected { error };
        }

        let ack = chunk_control_message(MSG_CHUNK_ACK, metadata.id, metadata.index);
        let key = (source.to_owned(), metadata.id);
        if self.groups.contains_key(&key) {
            return self.add_to_existing(key, metadata, msg, ack, error);
        }
        self.add_new_group(key, metadata, msg, ack, error)
    }

    fn add_new_group(
        &mut self,
        key: (String, u64),
        metadata: ChunkMetadata,
        msg: Message,
        ack: Message,
        error: Option<Message>,
    ) -> AssemblyOutcome {
        if self.groups.len() >= self.limits.max_groups {
            return AssemblyOutcome::Rejected { error };
        }
        let slot_bytes =
            match usize::from(metadata.count).checked_mul(std::mem::size_of::<Option<Vec<u8>>>()) {
                Some(bytes) => bytes,
                None => return AssemblyOutcome::Rejected { error },
            };
        let base_bytes = match slot_bytes.checked_add(metadata.common_var_data.len()) {
            Some(bytes) => bytes,
            None => return AssemblyOutcome::Rejected { error },
        };
        let memory_bytes = match base_bytes.checked_add(msg.body.len()) {
            Some(bytes) => bytes,
            None => return AssemblyOutcome::Rejected { error },
        };
        if self
            .retained_bytes
            .checked_add(memory_bytes)
            .is_none_or(|bytes| bytes > self.limits.max_bytes)
            || complete_wire_len(metadata.common_var_data.len(), msg.body.len())
                .is_none_or(|bytes| bytes > self.limits.max_message_bytes)
        {
            return AssemblyOutcome::Rejected { error };
        }

        let immediate_ack = (metadata.index + 1 < metadata.count).then_some(ack);
        let final_ack = chunk_control_message(MSG_CHUNK_ACK, metadata.id, metadata.count - 1);
        let mut chunks = vec![None; usize::from(metadata.count)];
        chunks[usize::from(metadata.index)] = Some(msg.body);
        let group = AssemblyGroup {
            message_type: msg.message_type,
            flags: msg.flags,
            common_var_data: metadata.common_var_data,
            chunks,
            payload_bytes: memory_bytes - base_bytes,
            memory_bytes,
            last_chunk_index: metadata.index,
            final_ack,
            last_updated: Instant::now(),
        };
        self.retained_bytes += memory_bytes;
        self.groups.insert(key.clone(), group);
        self.finish_or_pending(&key, immediate_ack, error)
    }

    fn add_to_existing(
        &mut self,
        key: (String, u64),
        metadata: ChunkMetadata,
        msg: Message,
        ack: Message,
        error: Option<Message>,
    ) -> AssemblyOutcome {
        let invalid = {
            let group = self.groups.get(&key).expect("group checked above");
            group.message_type != msg.message_type
                || group.flags != msg.flags
                || group.chunks.len() != usize::from(metadata.count)
                || group.common_var_data != metadata.common_var_data
        };
        if invalid {
            self.remove_group(&key);
            return AssemblyOutcome::Rejected { error };
        }

        let index = usize::from(metadata.index);
        let duplicate = self
            .groups
            .get(&key)
            .and_then(|group| group.chunks[index].as_ref())
            .map(|body| body == &msg.body);
        match duplicate {
            Some(true) => {
                if let Some(group) = self.groups.get_mut(&key) {
                    group.last_updated = Instant::now();
                    group.last_chunk_index = metadata.index;
                }
                let immediate_ack = (metadata.index + 1 < metadata.count).then_some(ack);
                return AssemblyOutcome::Pending { ack: immediate_ack };
            }
            Some(false) => {
                self.remove_group(&key);
                return AssemblyOutcome::Rejected { error };
            }
            None => {}
        }

        let (next_group_bytes, next_total_bytes, next_wire_len) = {
            let group = self.groups.get(&key).expect("group checked above");
            (
                group.memory_bytes.checked_add(msg.body.len()),
                self.retained_bytes.checked_add(msg.body.len()),
                group
                    .payload_bytes
                    .checked_add(msg.body.len())
                    .and_then(|body| complete_wire_len(group.common_var_data.len(), body)),
            )
        };
        if next_group_bytes.is_none()
            || next_total_bytes.is_none_or(|bytes| bytes > self.limits.max_bytes)
            || next_wire_len.is_none_or(|bytes| bytes > self.limits.max_message_bytes)
        {
            self.remove_group(&key);
            return AssemblyOutcome::Rejected { error };
        }

        let body_len = msg.body.len();
        if let Some(group) = self.groups.get_mut(&key) {
            group.chunks[index] = Some(msg.body);
            group.payload_bytes += body_len;
            group.memory_bytes += body_len;
            group.last_chunk_index = metadata.index;
            group.last_updated = Instant::now();
        }
        self.retained_bytes += body_len;
        let immediate_ack = (metadata.index + 1 < metadata.count).then_some(ack);
        self.finish_or_pending(&key, immediate_ack, error)
    }

    fn finish_or_pending(
        &mut self,
        key: &(String, u64),
        ack: Option<Message>,
        error: Option<Message>,
    ) -> AssemblyOutcome {
        let complete = self
            .groups
            .get(key)
            .is_some_and(|group| group.chunks.iter().all(Option::is_some));
        if !complete {
            return AssemblyOutcome::Pending { ack };
        }
        let Some(group) = self.take_group(key) else {
            return AssemblyOutcome::Rejected { error };
        };
        let mut body = Vec::with_capacity(group.payload_bytes);
        for chunk in group.chunks {
            let Some(chunk) = chunk else {
                return AssemblyOutcome::Rejected { error };
            };
            body.extend_from_slice(&chunk);
        }
        AssemblyOutcome::Complete {
            message: Message {
                message_type: group.message_type,
                flags: group.flags,
                var_data: group.common_var_data,
                body,
            },
            ack,
            final_ack: Some(group.final_ack),
        }
    }

    pub fn expire(&mut self, now: Instant) -> Vec<ExpiredAssembly> {
        let expired_keys: Vec<_> = self
            .groups
            .iter()
            .filter(|(_, group)| {
                now.saturating_duration_since(group.last_updated) >= self.limits.ttl
            })
            .map(|(key, _)| key.clone())
            .collect();
        let mut expired = Vec::with_capacity(expired_keys.len());
        for key in expired_keys {
            if let Some(group) = self.take_group(&key) {
                expired.push(ExpiredAssembly {
                    source: key.0,
                    error: chunk_control_message(MSG_CHUNK_ERROR, key.1, group.last_chunk_index),
                });
            }
        }
        expired
    }

    fn remove_group(&mut self, key: &(String, u64)) {
        let _ = self.take_group(key);
    }

    fn take_group(&mut self, key: &(String, u64)) -> Option<AssemblyGroup> {
        let group = self.groups.remove(key)?;
        self.retained_bytes = self.retained_bytes.saturating_sub(group.memory_bytes);
        Some(group)
    }

    #[cfg(test)]
    fn groups_len(&self) -> usize {
        self.groups.len()
    }

    #[cfg(test)]
    fn retained_bytes(&self) -> usize {
        self.retained_bytes
    }
}

fn complete_wire_len(var_data_len: usize, body_len: usize) -> Option<usize> {
    HEADER_SIZE
        .checked_add(var_data_len)
        .and_then(|len| len.checked_add(body_len))
}

fn parse_chunk_metadata(var_data: &[u8]) -> Result<Option<ChunkMetadata>, &'static str> {
    match crate::validation::chunk_classify(var_data) {
        crate::validation::ChunkClass::None => Ok(None),
        crate::validation::ChunkClass::Complete { id, index, count, .. } => {
            let common_var_data = crate::validation::strip_chunk(var_data);
            Ok(Some(ChunkMetadata {
                id,
                index,
                count,
                common_var_data,
            }))
        }
        crate::validation::ChunkClass::Incomplete => Err("incomplete chunk metadata"),
        crate::validation::ChunkClass::Invalid => Err("invalid chunk metadata"),
    }
}

/// Remove chunk TLV fields from var_data.
pub fn strip_chunk_metadata(var_data: &[u8]) -> Vec<u8> {
    crate::validation::strip_chunk(var_data)
}

/// Build a chunk ack message for the given chunked message.
pub fn ack_message(msg: &Message) -> Option<Message> {
    let (id, index) = chunk_control_fields(msg)?;
    Some(chunk_control_message(MSG_CHUNK_ACK, id, index))
}

/// Return a string key for matching chunk acks to pending sends.
pub fn ack_key(msg: &Message) -> Option<String> {
    let (chunk_id, chunk_index) = chunk_control_fields(msg)?;
    Some(format!("{chunk_id}:{chunk_index}"))
}

// ── Borrowed Raw-Frame Helpers ───────────────────────────────────────────
//
// The receive hot path classifies and queues raw wire frames without
// allocating `Message` (and without the old unpack->repack round trip). These
// helpers borrow the frame bytes; only chunked frames pay the unpack cost.

/// Borrowed wire header: `(message_type, flags, var_data_len, body_len)`.
///
/// Delegates to the C single source of truth (exact buffer-length match).
pub fn raw_frame_layout(data: &[u8]) -> Option<(u8, u8, usize, usize)> {
    crate::validation::frame_layout(data)
}

/// Re-export of the C chunk classification outcome.
pub use crate::validation::ChunkClass;

/// Single-pass frame classification for the receive hot path: exact
/// frame-header/length validation AND var_data chunk-metadata classify in one
/// FFI crossing (copy-neutral). Returns `(message_type, flags, var_data_len,
/// body_len, chunk_class)`.
pub fn classify_frame(raw: &[u8]) -> Option<(u8, u8, usize, usize, ChunkClass)> {
    crate::validation::classify_frame(raw)
        .map(|f| (f.message_type, f.flags, f.var_data_len, f.body_len, f.chunk))
}

/// True when the var_data block is well-formed and carries no chunk
/// metadata TLV at all. Such a frame is a complete single-frame message and
/// can be queued verbatim (zero-copy) instead of being unpacked/repacked.
pub fn raw_is_single_frame(var_data: &[u8]) -> bool {
    matches!(crate::validation::chunk_classify(var_data), crate::validation::ChunkClass::None)
}

fn chunk_control_fields_from_var_data(var_data: &[u8]) -> Option<(u64, u16)> {
    crate::validation::ack_fields(var_data)
}

fn chunk_control_fields(msg: &Message) -> Option<(u64, u16)> {
    chunk_control_fields_from_var_data(&msg.var_data)
}

fn chunk_control_message(message_type: u8, chunk_id: u64, chunk_index: u16) -> Message {
    let mut vd = VarDataBuilder::new();
    vd.chunk_id(chunk_id).chunk_index(chunk_index);
    Message {
        message_type,
        flags: 0,
        var_data: vd.build(),
        body: Vec::new(),
    }
}

// ── Inbound Processing ───────────────────────────────────────────────────

/// High-level classification returned by [`InboundProcessor`].
pub enum InboundResultKind {
    ChunkAck(Message),
    ChunkError(Message),
    Pending,
    Complete(Message),
    Rejected,
}

/// Result of processing one inbound protocol message.
pub struct InboundResult {
    /// ACK that can be sent immediately for a non-final chunk.
    pub ack_message: Option<Message>,
    /// ACK for a final chunk. It must only be sent after application queueing.
    pub final_ack_message: Option<Message>,
    /// Error response for malformed or rejected chunk state.
    pub error_message: Option<Message>,
    /// Control, pending, or complete business/control message.
    pub kind: InboundResultKind,
}

/// Unified inbound protocol processor.
///
/// This keeps control messages (`chunk.ack` and `chunk.error`) out of the
/// assembler, ACKs regular chunks immediately, and only emits application
/// messages after they are complete.
pub struct InboundProcessor {
    assembler: ChunkAssembler,
}

impl Default for InboundProcessor {
    fn default() -> Self {
        Self::new()
    }
}

impl InboundProcessor {
    pub fn new() -> Self {
        Self {
            assembler: ChunkAssembler::new(),
        }
    }

    pub fn with_limits(limits: ReassemblyLimits) -> Self {
        Self {
            assembler: ChunkAssembler::with_limits(limits),
        }
    }

    pub fn process(&mut self, source: &str, msg: Message) -> InboundResult {
        match msg.message_type {
            MSG_CHUNK_ACK | MSG_CHUNK_ERROR
                if msg.flags != 0 || !msg.body.is_empty() || ack_key(&msg).is_none() =>
            {
                InboundResult {
                    ack_message: None,
                    final_ack_message: None,
                    error_message: None,
                    kind: InboundResultKind::Rejected,
                }
            }
            MSG_CHUNK_ACK => InboundResult {
                ack_message: None,
                final_ack_message: None,
                error_message: None,
                kind: InboundResultKind::ChunkAck(msg),
            },
            MSG_CHUNK_ERROR => InboundResult {
                ack_message: None,
                final_ack_message: None,
                error_message: None,
                kind: InboundResultKind::ChunkError(msg),
            },
            _ => match self.assembler.add_from(source, msg) {
                AssemblyOutcome::Pending { ack } => InboundResult {
                    ack_message: ack,
                    final_ack_message: None,
                    error_message: None,
                    kind: InboundResultKind::Pending,
                },
                AssemblyOutcome::Complete {
                    message,
                    ack,
                    final_ack,
                } => InboundResult {
                    ack_message: ack,
                    final_ack_message: final_ack,
                    error_message: None,
                    kind: InboundResultKind::Complete(message),
                },
                AssemblyOutcome::Rejected { error } => InboundResult {
                    ack_message: None,
                    final_ack_message: None,
                    error_message: error,
                    kind: InboundResultKind::Rejected,
                },
            },
        }
    }

    pub fn expire(&mut self, now: Instant) -> Vec<ExpiredAssembly> {
        self.assembler.expire(now)
    }
}

// ── Business Message Builders ─────────────────────────────────────────────

/// Build a business request message.
pub fn build_business_request(
    module: &str,
    action: &str,
    request_id: &str,
    connection_id: &str,
    context_json: Option<&[u8]>,
    payload: &[u8],
) -> Message {
    let mut vd = VarDataBuilder::new();
    vd.module(module).action(action).request_id(request_id);
    if !connection_id.is_empty() {
        vd.connection_id(connection_id);
    }
    if let Some(ctx) = context_json {
        vd.context(ctx);
    }
    Message {
        message_type: MSG_BUSINESS_REQUEST,
        flags: 0,
        var_data: vd.build(),
        body: payload.to_vec(),
    }
}

/// Build a business response message.
pub fn build_business_response(
    module: &str,
    action: &str,
    request_id: &str,
    connection_id: &str,
    payload: &[u8],
    streaming: bool,
) -> Message {
    let mut vd = VarDataBuilder::new();
    vd.module(module).action(action).request_id(request_id);
    if !connection_id.is_empty() {
        vd.connection_id(connection_id);
    }
    if streaming {
        vd.streaming(true);
    }
    Message {
        message_type: MSG_BUSINESS_RESPONSE,
        flags: 0,
        var_data: vd.build(),
        body: payload.to_vec(),
    }
}

/// Pack a business response directly into a caller-owned wire buffer.
///
/// Unlike `build_business_response` followed by `pack`, this copies the body
/// only once, directly into its final wire representation.
pub fn pack_business_response_into(
    out: &mut Vec<u8>,
    module: &str,
    action: &str,
    request_id: &str,
    connection_id: &str,
    payload: &[u8],
    streaming: bool,
) -> Result<(), &'static str> {
    let mut vd = VarDataBuilder::new();
    vd.module(module).action(action).request_id(request_id);
    if !connection_id.is_empty() {
        vd.connection_id(connection_id);
    }
    if streaming {
        vd.streaming(true);
    }
    let var_data = vd.try_build()?;
    let message = Message {
        message_type: MSG_BUSINESS_RESPONSE,
        flags: 0,
        var_data,
        body: Vec::new(),
    };
    let vd_len = u16::try_from(message.var_data.len()).map_err(|_| "var_data exceeds u16")?;
    let body_len = u32::try_from(payload.len()).map_err(|_| "body exceeds u32")?;
    let total_len = HEADER_SIZE
        .checked_add(message.var_data.len())
        .and_then(|len| len.checked_add(payload.len()))
        .ok_or("message length overflow")?;
    out.clear();
    out.try_reserve(total_len)
        .map_err(|_| "message buffer allocation failed")?;
    out.push(message.message_type);
    out.push(message.flags);
    out.extend_from_slice(&vd_len.to_le_bytes());
    out.extend_from_slice(&body_len.to_le_bytes());
    out.extend_from_slice(&message.var_data);
    out.extend_from_slice(payload);
    Ok(())
}

/// Parse and validate all business fields in one TLV scan.
pub fn parse_business_fields(msg: &Message) -> Result<BusinessFields, &'static str> {
    let mut fields = BusinessFields::default();
    let mut seen = [false; 256];
    let mut offset = 0usize;
    while offset < msg.var_data.len() {
        let header_end = offset.checked_add(3).ok_or("TLV offset overflow")?;
        if header_end > msg.var_data.len() {
            return Err("truncated TLV header");
        }
        let tag = msg.var_data[offset];
        let len = u16::from_le_bytes([msg.var_data[offset + 1], msg.var_data[offset + 2]]) as usize;
        let value_end = header_end.checked_add(len).ok_or("TLV length overflow")?;
        if value_end > msg.var_data.len() {
            return Err("truncated TLV value");
        }
        let value = &msg.var_data[header_end..value_end];
        let string = |value: &[u8]| {
            std::str::from_utf8(value)
                .map(str::to_owned)
                .map_err(|_| "business field is not UTF-8")
        };
        if seen[usize::from(tag)] {
            offset = value_end;
            continue;
        }
        seen[usize::from(tag)] = true;
        match tag {
            TAG_MODULE => fields.module = string(value)?,
            TAG_ACTION => fields.action = string(value)?,
            TAG_REQUEST_ID => fields.request_id = string(value)?,
            TAG_CONNECTION_ID => fields.connection_id = string(value)?,
            TAG_STREAMING if value.len() == 1 => fields.streaming = value[0] != 0,
            TAG_STREAMING => return Err("invalid streaming field"),
            TAG_WORKFLOW_ID => fields.workflow_id = Some(string(value)?),
            TAG_TASK_UUID => fields.task_uuid = Some(string(value)?),
            TAG_STEP_ID => fields.step_id = Some(string(value)?),
            TAG_ACTOR_UUID => fields.actor_uuid = Some(string(value)?),
            TAG_TRACE_ID => fields.trace_id = Some(string(value)?),
            TAG_CONTEXT => fields.context = Some(value.to_vec()),
            _ => {}
        }
        offset = value_end;
    }
    Ok(fields)
}

/// Extract business fields from a message.
///
/// This compatibility helper preserves the historical fail-closed defaults;
/// new code should use [`parse_business_fields`] and handle malformed TLV.
pub fn extract_business_fields(msg: &Message) -> BusinessFields {
    parse_business_fields(msg).unwrap_or_default()
}

/// Extracted business fields from a message.
#[derive(Debug, Clone, Default)]
pub struct BusinessFields {
    pub module: String,
    pub action: String,
    pub request_id: String,
    pub connection_id: String,
    pub streaming: bool,
    /// Workflow identifier (optional metadata).
    pub workflow_id: Option<String>,
    /// Task UUID (optional metadata).
    pub task_uuid: Option<String>,
    /// Step identifier (optional metadata).
    pub step_id: Option<String>,
    /// Actor UUID (optional metadata).
    pub actor_uuid: Option<String>,
    /// Trace identifier for distributed tracing (optional metadata).
    pub trace_id: Option<String>,
    /// Trusted request context bytes, copied once while scanning the TLV block.
    pub context: Option<Vec<u8>>,
}

// ── Tests ─────────────────────────────────────────────────────────────────

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn pack_unpack_round_trip() {
        let msg = Message {
            message_type: MSG_BUSINESS_REQUEST,
            flags: 0,
            var_data: VarDataBuilder::new()
                .module("admin")
                .action("auth.login")
                .request_id("r1")
                .build(),
            body: b"hello".to_vec(),
        };
        let packed = pack(&msg).unwrap();
        let unpacked = unpack(&packed).unwrap();
        assert_eq!(unpacked.message_type, MSG_BUSINESS_REQUEST);
        assert_eq!(
            tlv_read_string(&unpacked.var_data, TAG_MODULE).as_deref(),
            Some("admin")
        );
        assert_eq!(
            tlv_read_string(&unpacked.var_data, TAG_ACTION).as_deref(),
            Some("auth.login")
        );
        assert_eq!(
            tlv_read_string(&unpacked.var_data, TAG_REQUEST_ID).as_deref(),
            Some("r1")
        );
        assert_eq!(unpacked.body, b"hello");
    }

    #[test]
    fn tlv_read_write_round_trip() {
        let mut vd = VarDataBuilder::new();
        vd.module("admin")
            .action("test.action")
            .request_id("req-1")
            .connection_id("conn-1")
            .streaming(true)
            .status(200)
            .is_text(true);
        let data = vd.build();
        assert_eq!(tlv_read_string(&data, TAG_MODULE).as_deref(), Some("admin"));
        assert_eq!(
            tlv_read_string(&data, TAG_ACTION).as_deref(),
            Some("test.action")
        );
        assert_eq!(
            tlv_read_string(&data, TAG_REQUEST_ID).as_deref(),
            Some("req-1")
        );
        assert_eq!(
            tlv_read_string(&data, TAG_CONNECTION_ID).as_deref(),
            Some("conn-1")
        );
        assert_eq!(tlv_read_byte(&data, TAG_STREAMING), Some(1));
        assert_eq!(tlv_read_u16(&data, TAG_STATUS), Some(200));
        assert_eq!(tlv_read_byte(&data, TAG_IS_TEXT), Some(1));
    }

    #[test]
    fn chunk_and_reassemble() {
        let msg = Message {
            message_type: MSG_BUSINESS_REQUEST,
            flags: 0,
            var_data: VarDataBuilder::new().module("admin").action("test").build(),
            body: vec![b'x'; 100_000],
        };
        let chunks = chunk(&msg, 4096).unwrap();
        assert!(chunks.len() > 1);

        let mut assembler = ChunkAssembler::new();
        let mut complete = None;
        for c in chunks {
            complete = assembler.add(c.message);
        }
        let complete = complete.unwrap();
        assert_eq!(complete.message_type, MSG_BUSINESS_REQUEST);
        assert_eq!(
            tlv_read_string(&complete.var_data, TAG_MODULE).as_deref(),
            Some("admin")
        );
        assert_eq!(complete.body.len(), 100_000);
    }

    #[test]
    fn build_business_request_has_correct_fields() {
        let msg = build_business_request(
            "admin",
            "auth.login",
            "req-1",
            "conn-1",
            Some(b"{\"host\":\"example.com\"}"),
            b"{\"action\":\"auth.login\"}",
        );
        assert_eq!(msg.message_type, MSG_BUSINESS_REQUEST);
        assert_eq!(
            tlv_read_string(&msg.var_data, TAG_MODULE).as_deref(),
            Some("admin")
        );
        assert_eq!(
            tlv_read_string(&msg.var_data, TAG_ACTION).as_deref(),
            Some("auth.login")
        );
        assert_eq!(
            tlv_read_string(&msg.var_data, TAG_REQUEST_ID).as_deref(),
            Some("req-1")
        );
        assert_eq!(
            tlv_read_string(&msg.var_data, TAG_CONNECTION_ID).as_deref(),
            Some("conn-1")
        );
        assert!(tlv_read_bytes(&msg.var_data, TAG_CONTEXT).is_some());
        assert_eq!(msg.body, b"{\"action\":\"auth.login\"}");
    }

    #[test]
    fn build_business_response_with_streaming() {
        let msg = build_business_response(
            "admin",
            "visitors.live",
            "req-2",
            "conn-2",
            b"{\"visitors\":[]}",
            true,
        );
        assert_eq!(msg.message_type, MSG_BUSINESS_RESPONSE);
        assert_eq!(tlv_read_byte(&msg.var_data, TAG_STREAMING), Some(1));
    }

    #[test]
    fn extract_business_fields_works() {
        let msg = build_business_request("admin", "test", "r1", "c1", None, b"payload");
        let fields = extract_business_fields(&msg);
        assert_eq!(fields.module, "admin");
        assert_eq!(fields.action, "test");
        assert_eq!(fields.request_id, "r1");
        assert_eq!(fields.connection_id, "c1");
        assert!(!fields.streaming);
        assert!(fields.workflow_id.is_none());
        assert!(fields.task_uuid.is_none());
    }

    #[test]
    fn parse_business_fields_validates_once_and_keeps_context() {
        let msg = build_business_request(
            "admin",
            "test",
            "r1",
            "c1",
            Some(br#"{"site_name":"acme"}"#),
            b"payload",
        );
        let fields = parse_business_fields(&msg).unwrap();
        assert_eq!(fields.module, "admin");
        assert_eq!(
            fields.context.as_deref(),
            Some(br#"{"site_name":"acme"}"#.as_slice())
        );

        let mut malformed = msg;
        malformed
            .var_data
            .extend_from_slice(&[TAG_ACTION, 4, 0, b'x']);
        assert_eq!(
            parse_business_fields(&malformed).unwrap_err(),
            "truncated TLV value"
        );

        let mut duplicate = VarDataBuilder::new();
        duplicate.module("first").module("second");
        let duplicate = Message {
            message_type: MSG_BUSINESS_REQUEST,
            flags: 0,
            var_data: duplicate.build(),
            body: Vec::new(),
        };
        assert_eq!(parse_business_fields(&duplicate).unwrap().module, "first");
    }

    #[test]
    fn direct_response_pack_is_wire_compatible_for_large_and_streaming_bodies() {
        for (size, streaming) in [(64 * 1024, false), (512 * 1024, true)] {
            let payload = vec![b'z'; size];
            let legacy = pack(&build_business_response(
                "public",
                "assets.read",
                "request-1",
                "connection-1",
                &payload,
                streaming,
            ))
            .unwrap();
            let mut direct = Vec::with_capacity(legacy.len());
            pack_business_response_into(
                &mut direct,
                "public",
                "assets.read",
                "request-1",
                "connection-1",
                &payload,
                streaming,
            )
            .unwrap();
            assert_eq!(direct, legacy);
        }
    }

    #[test]
    fn pack_into_reuses_allocation_and_matches_pack() {
        let message = build_business_response("admin", "test", "r1", "c1", &[7; 4096], false);
        let expected = pack(&message).unwrap();
        let mut reused = Vec::with_capacity(expected.len() * 2);
        let capacity = reused.capacity();
        pack_into(&message, &mut reused).unwrap();
        assert_eq!(reused, expected);
        assert_eq!(reused.capacity(), capacity);
    }

    #[test]
    fn wire_buffer_pool_is_bounded_and_reuses_size_classes() {
        let pool = WireBufferPool::new([4096, 64 * 1024, 512 * 1024], 512 * 1024).unwrap();
        let first = pool.acquire(60_000);
        assert_eq!(first.capacity(), 64 * 1024);
        pool.release(first);
        assert_eq!(pool.stats().retained_bytes, 64 * 1024);
        let reused = pool.acquire(60_000);
        assert_eq!(reused.capacity(), 64 * 1024);
        assert_eq!(pool.stats().reuses, 1);
    }

    #[test]
    fn workflow_tlv_round_trip() {
        let mut vd = VarDataBuilder::new();
        vd.module("agent")
            .action("ml.process")
            .request_id("wf-1")
            .workflow_id("wf-abc-123")
            .task_uuid("task-uuid-456")
            .step_id("step-7")
            .actor_uuid("actor-uuid-890")
            .trace_id("trace-id-xyz");
        let data = vd.build();
        assert_eq!(
            tlv_read_string(&data, TAG_WORKFLOW_ID).as_deref(),
            Some("wf-abc-123")
        );
        assert_eq!(
            tlv_read_string(&data, TAG_TASK_UUID).as_deref(),
            Some("task-uuid-456")
        );
        assert_eq!(
            tlv_read_string(&data, TAG_STEP_ID).as_deref(),
            Some("step-7")
        );
        assert_eq!(
            tlv_read_string(&data, TAG_ACTOR_UUID).as_deref(),
            Some("actor-uuid-890")
        );
        assert_eq!(
            tlv_read_string(&data, TAG_TRACE_ID).as_deref(),
            Some("trace-id-xyz")
        );

        // Verify extract_business_fields picks them up
        let msg = Message {
            message_type: MSG_BUSINESS_REQUEST,
            flags: 0,
            var_data: data,
            body: Vec::new(),
        };
        let fields = extract_business_fields(&msg);
        assert_eq!(fields.workflow_id.as_deref(), Some("wf-abc-123"));
        assert_eq!(fields.task_uuid.as_deref(), Some("task-uuid-456"));
        assert_eq!(fields.step_id.as_deref(), Some("step-7"));
        assert_eq!(fields.actor_uuid.as_deref(), Some("actor-uuid-890"));
        assert_eq!(fields.trace_id.as_deref(), Some("trace-id-xyz"));
    }

    #[test]
    fn ack_message_round_trip() {
        let msg = Message {
            message_type: MSG_BUSINESS_REQUEST,
            flags: 0,
            var_data: VarDataBuilder::new().chunk_id(42).chunk_index(3).build(),
            body: Vec::new(),
        };
        let ack = ack_message(&msg).unwrap();
        assert_eq!(ack.message_type, MSG_CHUNK_ACK);
        assert_eq!(ack_key(&ack).as_deref(), Some("42:3"));
    }

    #[test]
    fn inbound_processor_keeps_ack_out_of_assembler() {
        let mut processor = InboundProcessor::new();
        let msg = build_business_response(
            "admin",
            "auth.login",
            "req-1",
            "ws-1",
            &[b'a'; 70_000],
            false,
        );
        let chunk = chunk(&msg, MAX_APP_PAYLOAD).unwrap().remove(0);
        let ack = ack_message(&chunk.message).unwrap();
        let key = ack_key(&ack);

        let result = processor.process("core", ack);
        assert!(result.ack_message.is_none());
        match result.kind {
            InboundResultKind::ChunkAck(msg) => {
                assert_eq!(ack_key(&msg), key);
            }
            _ => panic!("expected chunk ack"),
        }
    }

    #[test]
    fn inbound_processor_acks_chunks_and_emits_complete_message() {
        let mut processor = InboundProcessor::new();
        let mut msg = Message::new(MSG_BUSINESS_RESPONSE);
        msg.var_data = VarDataBuilder::new()
            .module("admin")
            .action("auth.login")
            .request_id("req-1")
            .build();
        msg.body = vec![b'z'; MAX_APP_PAYLOAD * 2];
        let chunks = chunk(&msg, MAX_APP_PAYLOAD).unwrap();

        let mut complete = None;
        for chunk in chunks {
            let chunk_key = ack_key(&chunk.message).unwrap();
            let result = processor.process("core", chunk.message);
            match result.kind {
                InboundResultKind::Pending => {
                    assert!(result.final_ack_message.is_none());
                    let ack = result.ack_message.expect("intermediate chunk ack");
                    assert_eq!(ack.message_type, MSG_CHUNK_ACK);
                    assert_eq!(ack_key(&ack).as_deref(), Some(chunk_key.as_str()));
                }
                InboundResultKind::Complete(msg) => {
                    assert!(result.ack_message.is_none());
                    let ack = result.final_ack_message.expect("deferred final chunk ack");
                    assert_eq!(ack.message_type, MSG_CHUNK_ACK);
                    assert_eq!(ack_key(&ack).as_deref(), Some(chunk_key.as_str()));
                    complete = Some(msg);
                }
                _ => panic!("unexpected inbound result"),
            }
        }

        let complete = complete.expect("complete business.response");
        assert_eq!(complete.message_type, MSG_BUSINESS_RESPONSE);
        assert_eq!(
            tlv_read_string(&complete.var_data, TAG_ACTION).as_deref(),
            Some("auth.login")
        );
        assert_eq!(complete.body, msg.body);
    }

    #[test]
    fn out_of_order_final_chunk_ack_stays_deferred_until_queueable_completion() {
        let mut processor = InboundProcessor::new();
        let mut original = Message::new(MSG_BUSINESS_RESPONSE);
        original.var_data = VarDataBuilder::new().action("reverse.final").build();
        original.body = vec![b'r'; MAX_APP_PAYLOAD * 2];
        let chunks = chunk(&original, MAX_APP_PAYLOAD).expect("chunk message");
        assert!(chunks.len() >= 3);
        let final_chunk = chunks.last().expect("final chunk").message.clone();
        let final_key = ack_key(&final_chunk).expect("final key");

        for message in [final_chunk.clone(), final_chunk] {
            let result = processor.process("core", message);
            assert!(matches!(result.kind, InboundResultKind::Pending));
            assert!(
                result.ack_message.is_none(),
                "final chunk must not be ACKed early"
            );
            assert!(result.final_ack_message.is_none());
        }

        for chunk in &chunks[..chunks.len() - 2] {
            let result = processor.process("core", chunk.message.clone());
            assert!(matches!(result.kind, InboundResultKind::Pending));
            assert_eq!(
                result.ack_message.as_ref().and_then(ack_key).as_deref(),
                ack_key(&chunk.message).as_deref()
            );
        }

        let completing = &chunks[chunks.len() - 2];
        let result = processor.process("core", completing.message.clone());
        let InboundResultKind::Complete(message) = result.kind else {
            panic!("non-final chunk should complete the reverse-order group");
        };
        assert_eq!(message, original);
        assert_eq!(
            result.ack_message.as_ref().and_then(ack_key).as_deref(),
            ack_key(&completing.message).as_deref()
        );
        assert_eq!(
            result
                .final_ack_message
                .as_ref()
                .and_then(ack_key)
                .as_deref(),
            Some(final_key.as_str())
        );
    }

    #[test]
    fn inbound_processor_keeps_chunk_error_out_of_assembler() {
        let mut processor = InboundProcessor::new();
        let msg = Message {
            message_type: MSG_CHUNK_ERROR,
            flags: 0,
            var_data: VarDataBuilder::new().chunk_id(42).chunk_index(0).build(),
            body: Vec::new(),
        };

        let result = processor.process("core", msg);
        assert!(result.ack_message.is_none());
        match result.kind {
            InboundResultKind::ChunkError(msg) => {
                assert_eq!(ack_key(&msg).as_deref(), Some("42:0"));
            }
            _ => panic!("expected chunk error"),
        }
    }

    #[test]
    fn unpack_rejects_short_data() {
        assert!(unpack(&[0, 1, 2]).is_err());
    }

    #[test]
    fn type_names_are_correct() {
        assert_eq!(type_name(MSG_BUSINESS_REQUEST), "business.request");
        assert_eq!(type_name(MSG_BUSINESS_RESPONSE), "business.response");
        assert_eq!(type_name(MSG_CHUNK_ACK), "chunk.ack");
        assert_eq!(type_name(MSG_CHUNK_ERROR), "chunk.error");
        assert_eq!(type_name(MSG_ROUTES_REFRESH), "routes.refresh");
        assert_eq!(type_name(MSG_CACHE_INVALIDATE), "cache.invalidate");
        assert_eq!(
            type_name(MSG_AGENT_REGISTRY_UPDATE),
            "agent.registry.update"
        );
        assert_eq!(type_name(MSG_AGENT_REQUEST), "agent.request");
        assert_eq!(type_name(MSG_AGENT_EVENT), "agent.event");
        assert_eq!(type_name(MSG_CONTROL), "control");
        assert_eq!(type_name(MSG_LOG_NOTICE), "log.notice");
        assert_eq!(type_name(0xFF), "unknown");
    }

    #[test]
    fn empty_body_message_round_trip() {
        let msg = Message {
            message_type: MSG_BUSINESS_REQUEST,
            flags: 0,
            var_data: VarDataBuilder::new()
                .module("admin")
                .action("ping")
                .request_id("p1")
                .build(),
            body: Vec::new(),
        };
        let packed = pack(&msg).unwrap();
        let unpacked = unpack(&packed).unwrap();
        assert_eq!(unpacked.message_type, MSG_BUSINESS_REQUEST);
        assert!(unpacked.body.is_empty());
        assert_eq!(
            tlv_read_string(&unpacked.var_data, TAG_MODULE).as_deref(),
            Some("admin")
        );
    }

    #[test]
    fn empty_var_data_message_round_trip() {
        let msg = Message {
            message_type: MSG_BUSINESS_RESPONSE,
            flags: 0,
            var_data: Vec::new(),
            body: b"response body".to_vec(),
        };
        let packed = pack(&msg).unwrap();
        let unpacked = unpack(&packed).unwrap();
        assert_eq!(unpacked.message_type, MSG_BUSINESS_RESPONSE);
        assert_eq!(unpacked.body, b"response body");
        assert!(unpacked.var_data.is_empty());
    }

    #[test]
    fn chunk_boundary_small_body_not_chunked() {
        // A message with body small enough to fit in one frame should not be chunked.
        let msg = Message {
            message_type: MSG_BUSINESS_REQUEST,
            flags: 0,
            var_data: VarDataBuilder::new()
                .module("test")
                .action("boundary")
                .build(),
            body: vec![b'y'; 100],
        };
        let chunks = chunk(&msg, MAX_APP_PAYLOAD).unwrap();
        assert_eq!(chunks.len(), 1);
        assert_eq!(chunks[0].message.body.len(), 100);
    }

    #[test]
    fn chunk_boundary_body_just_over_budget_gets_two_chunks() {
        // A body that is just over the budget should split into exactly 2 chunks.
        let vd = VarDataBuilder::new()
            .module("test")
            .action("boundary")
            .build();
        let chunk_vd_overhead: usize = 21; // chunk_id + chunk_index + chunk_count TLV overhead
        let body_budget = MAX_APP_PAYLOAD
            .saturating_sub(HEADER_SIZE)
            .saturating_sub(vd.len())
            .saturating_sub(chunk_vd_overhead);
        let msg = Message {
            message_type: MSG_BUSINESS_REQUEST,
            flags: 0,
            var_data: vd,
            body: vec![b'y'; body_budget + 1],
        };
        let chunks = chunk(&msg, MAX_APP_PAYLOAD).unwrap();
        assert_eq!(chunks.len(), 2);
        assert_eq!(chunks[0].message.body.len(), body_budget);
        assert_eq!(chunks[1].message.body.len(), 1);
    }

    #[test]
    fn chunk_preserves_all_tlv_metadata() {
        let msg = Message {
            message_type: MSG_BUSINESS_REQUEST,
            flags: 0,
            var_data: VarDataBuilder::new()
                .module("agent")
                .action("ml.process")
                .request_id("wf-chunk-1")
                .workflow_id("wf-abc")
                .task_uuid("task-123")
                .step_id("step-1")
                .actor_uuid("actor-1")
                .trace_id("trace-1")
                .build(),
            body: vec![b'c'; 100_000],
        };
        let chunks = chunk(&msg, 4096).unwrap();
        assert!(chunks.len() > 1);

        // Reassemble
        let mut assembler = ChunkAssembler::new();
        let mut complete = None;
        for c in chunks {
            complete = assembler.add(c.message);
        }
        let complete = complete.unwrap();

        // All workflow metadata must survive chunking.
        assert_eq!(
            tlv_read_string(&complete.var_data, TAG_MODULE).as_deref(),
            Some("agent")
        );
        assert_eq!(
            tlv_read_string(&complete.var_data, TAG_ACTION).as_deref(),
            Some("ml.process")
        );
        assert_eq!(
            tlv_read_string(&complete.var_data, TAG_REQUEST_ID).as_deref(),
            Some("wf-chunk-1")
        );
        assert_eq!(
            tlv_read_string(&complete.var_data, TAG_WORKFLOW_ID).as_deref(),
            Some("wf-abc")
        );
        assert_eq!(
            tlv_read_string(&complete.var_data, TAG_TASK_UUID).as_deref(),
            Some("task-123")
        );
        assert_eq!(
            tlv_read_string(&complete.var_data, TAG_STEP_ID).as_deref(),
            Some("step-1")
        );
        assert_eq!(
            tlv_read_string(&complete.var_data, TAG_ACTOR_UUID).as_deref(),
            Some("actor-1")
        );
        assert_eq!(
            tlv_read_string(&complete.var_data, TAG_TRACE_ID).as_deref(),
            Some("trace-1")
        );
        assert_eq!(complete.body.len(), 100_000);
    }

    #[test]
    fn extract_business_fields_with_all_workflow_metadata() {
        let msg = Message {
            message_type: MSG_BUSINESS_REQUEST,
            flags: 0,
            var_data: VarDataBuilder::new()
                .module("agent")
                .action("content_import.run")
                .request_id("req-full")
                .connection_id("conn-1")
                .workflow_id("wf-test")
                .task_uuid("task-test")
                .step_id("step-test")
                .actor_uuid("actor-test")
                .trace_id("trace-test")
                .build(),
            body: b"{}".to_vec(),
        };
        let fields = extract_business_fields(&msg);
        assert_eq!(fields.module, "agent");
        assert_eq!(fields.action, "content_import.run");
        assert_eq!(fields.request_id, "req-full");
        assert_eq!(fields.connection_id, "conn-1");
        assert_eq!(fields.workflow_id.as_deref(), Some("wf-test"));
        assert_eq!(fields.task_uuid.as_deref(), Some("task-test"));
        assert_eq!(fields.step_id.as_deref(), Some("step-test"));
        assert_eq!(fields.actor_uuid.as_deref(), Some("actor-test"));
        assert_eq!(fields.trace_id.as_deref(), Some("trace-test"));
    }

    #[test]
    fn pack_unpack_preserves_flags() {
        let msg = Message {
            message_type: MSG_BUSINESS_RESPONSE,
            flags: 0x42,
            var_data: VarDataBuilder::new().module("test").build(),
            body: b"flags-test".to_vec(),
        };
        let packed = pack(&msg).unwrap();
        let unpacked = unpack(&packed).unwrap();
        assert_eq!(unpacked.flags, 0x42);
    }

    #[test]
    fn tlv_read_nonexistent_tag_returns_none() {
        let vd = VarDataBuilder::new().module("admin").build();
        assert!(tlv_read_string(&vd, TAG_TRACE_ID).is_none());
        assert!(tlv_read_u16(&vd, TAG_STATUS).is_none());
        assert!(tlv_read_byte(&vd, TAG_STREAMING).is_none());
    }

    #[test]
    fn business_request_with_empty_context() {
        let msg = build_business_request("admin", "test", "r1", "c1", None, b"body");
        // No context bytes should not crash.
        assert!(tlv_read_bytes(&msg.var_data, TAG_CONTEXT).is_none());
        assert_eq!(msg.body, b"body");
    }

    #[test]
    fn business_request_with_context() {
        let ctx = b"{\"host\":\"example.com\",\"ip\":\"127.0.0.1\"}";
        let msg = build_business_request("admin", "test", "r1", "c1", Some(ctx), b"body");
        let read_ctx = tlv_read_bytes(&msg.var_data, TAG_CONTEXT).unwrap();
        assert_eq!(read_ctx, ctx);
    }

    #[test]
    fn chunk_assembler_ignores_duplicate_chunks() {
        let msg = Message {
            message_type: MSG_BUSINESS_REQUEST,
            flags: 0,
            var_data: VarDataBuilder::new().module("test").action("dup").build(),
            body: vec![b'd'; 10_000],
        };
        let chunks = chunk(&msg, 4096).unwrap();
        let mut assembler = ChunkAssembler::new();
        let mut complete = None;
        // Add all chunks, including a duplicate of the first.
        for (i, c) in chunks.iter().enumerate() {
            complete = assembler.add(c.message.clone());
            if i == 0 {
                // Add duplicate first chunk.
                complete = assembler.add(c.message.clone());
            }
        }
        // Should still get a complete message.
        assert!(complete.is_some());
    }

    #[test]
    fn unpack_rejects_trailing_bytes_and_malformed_tlvs() {
        let msg = Message::new(MSG_CONTROL);
        let mut packed = pack(&msg).expect("pack control");
        packed.push(0);
        assert_eq!(unpack(&packed), Err("frame has trailing data"));

        let malformed = Message {
            message_type: MSG_CONTROL,
            flags: 0,
            var_data: vec![TAG_ACTION, 2, 0, b'x'],
            body: Vec::new(),
        };
        assert!(pack(&malformed).is_err());
        assert!(tlv_read_bytes(&malformed.var_data, TAG_ACTION).is_none());
    }

    #[test]
    fn oversized_tlv_builder_never_truncates_a_length() {
        let mut builder = VarDataBuilder::new();
        builder.put_bytes(TAG_JSON_DATA, &vec![0; usize::from(u16::MAX) + 1]);
        assert!(builder.try_build().is_err());

        let mut compatibility_builder = VarDataBuilder::new();
        compatibility_builder.put_bytes(TAG_JSON_DATA, &vec![0; usize::from(u16::MAX) + 1]);
        let msg = Message {
            message_type: MSG_CONTROL,
            flags: 0,
            var_data: compatibility_builder.build(),
            body: Vec::new(),
        };
        assert!(pack(&msg).is_err());
    }

    #[test]
    fn chunk_rejects_metadata_that_cannot_fit_the_frame() {
        let msg = Message {
            message_type: MSG_BUSINESS_REQUEST,
            flags: 0,
            var_data: VarDataBuilder::new()
                .put_bytes(TAG_CONTEXT, &[b'x'; 128])
                .build(),
            body: vec![1],
        };
        assert!(chunk(&msg, 100).is_err());
    }

    #[test]
    fn reassembly_key_includes_source_service() {
        let original = Message {
            message_type: MSG_BUSINESS_REQUEST,
            flags: 7,
            var_data: VarDataBuilder::new().action("source-key").build(),
            body: vec![b'a'; 10_000],
        };
        let chunks = chunk(&original, 4096).expect("chunk message");
        let mut assembler = ChunkAssembler::new();

        assert!(matches!(
            assembler.add_from("sender-a", chunks[0].message.clone()),
            AssemblyOutcome::Pending { .. }
        ));

        let mut from_b = None;
        for chunk in chunks.iter().rev() {
            let mut message = chunk.message.clone();
            message.body.fill(b'b');
            if let AssemblyOutcome::Complete { message, .. } =
                assembler.add_from("sender-b", message)
            {
                from_b = Some(message);
            }
        }
        assert_eq!(from_b.expect("sender-b complete").body, vec![b'b'; 10_000]);

        let mut from_a = None;
        for chunk in &chunks[1..] {
            if let AssemblyOutcome::Complete { message, .. } =
                assembler.add_from("sender-a", chunk.message.clone())
            {
                from_a = Some(message);
            }
        }
        assert_eq!(from_a.expect("sender-a complete"), original);
    }

    #[test]
    fn inconsistent_metadata_and_conflicting_duplicates_release_the_group() {
        let original = Message {
            message_type: MSG_BUSINESS_REQUEST,
            flags: 0,
            var_data: VarDataBuilder::new().action("consistent").build(),
            body: vec![b'x'; 10_000],
        };
        let chunks = chunk(&original, 4096).expect("chunk message");
        let mut assembler = ChunkAssembler::new();
        assert!(matches!(
            assembler.add_from("sender", chunks[0].message.clone()),
            AssemblyOutcome::Pending { .. }
        ));
        let mut conflicting = chunks[0].message.clone();
        conflicting.body[0] ^= 1;
        assert!(matches!(
            assembler.add_from("sender", conflicting),
            AssemblyOutcome::Rejected { error: Some(_) }
        ));
        assert_eq!(assembler.groups_len(), 0);
        assert_eq!(assembler.retained_bytes(), 0);

        assert!(matches!(
            assembler.add_from("sender", chunks[0].message.clone()),
            AssemblyOutcome::Pending { .. }
        ));
        let mut mismatched = chunks[1].message.clone();
        mismatched
            .var_data
            .extend_from_slice(&VarDataBuilder::new().trace_id("changed").build());
        assert!(matches!(
            assembler.add_from("sender", mismatched),
            AssemblyOutcome::Rejected { error: Some(_) }
        ));
        assert_eq!(assembler.groups_len(), 0);
        assert_eq!(assembler.retained_bytes(), 0);
    }

    #[test]
    fn incomplete_metadata_rejects_and_releases_an_existing_group() {
        let original = Message {
            message_type: MSG_BUSINESS_REQUEST,
            flags: 0,
            var_data: Vec::new(),
            body: vec![b'x'; 10_000],
        };
        let chunks = chunk(&original, 4096).expect("chunk message");
        let mut assembler = ChunkAssembler::new();
        assert!(matches!(
            assembler.add_from("sender", chunks[0].message.clone()),
            AssemblyOutcome::Pending { .. }
        ));
        let (id, index) = chunk_control_fields(&chunks[1].message).expect("chunk fields");
        let malformed = Message {
            message_type: original.message_type,
            flags: original.flags,
            var_data: VarDataBuilder::new()
                .chunk_id(id)
                .chunk_index(index)
                .build(),
            body: chunks[1].message.body.clone(),
        };
        assert!(matches!(
            assembler.add_from("sender", malformed),
            AssemblyOutcome::Rejected { error: Some(_) }
        ));
        assert_eq!(assembler.groups_len(), 0);
        assert_eq!(assembler.retained_bytes(), 0);
    }

    #[test]
    fn reassembly_expires_and_releases_memory() {
        let limits = ReassemblyLimits {
            ttl: Duration::from_millis(1),
            max_groups: 4,
            max_bytes: 1024 * 1024,
            max_message_bytes: 1024 * 1024,
        };
        let original = Message {
            message_type: MSG_BUSINESS_REQUEST,
            flags: 0,
            var_data: Vec::new(),
            body: vec![b'x'; 10_000],
        };
        let chunks = chunk(&original, 4096).expect("chunk message");
        let mut assembler = ChunkAssembler::with_limits(limits);
        assembler.add_from("sender", chunks[0].message.clone());
        let expected_key = ack_key(&chunks[0].message);

        let expired = assembler.expire(Instant::now() + Duration::from_secs(1));
        assert_eq!(expired.len(), 1);
        assert_eq!(expired[0].source, "sender");
        assert_eq!(ack_key(&expired[0].error), expected_key);
        assert_eq!(assembler.groups_len(), 0);
        assert_eq!(assembler.retained_bytes(), 0);
    }

    #[test]
    fn reassembly_group_and_memory_limits_reject_without_retaining_state() {
        let original = Message {
            message_type: MSG_BUSINESS_REQUEST,
            flags: 0,
            var_data: Vec::new(),
            body: vec![b'x'; 10_000],
        };
        let chunks = chunk(&original, 4096).expect("chunk message");
        let mut no_memory = ChunkAssembler::with_limits(ReassemblyLimits {
            ttl: Duration::from_secs(1),
            max_groups: 1,
            max_bytes: 1,
            max_message_bytes: 1024 * 1024,
        });
        assert!(matches!(
            no_memory.add_from("sender", chunks[0].message.clone()),
            AssemblyOutcome::Rejected { error: Some(_) }
        ));
        assert_eq!(no_memory.retained_bytes(), 0);

        let mut no_groups = ChunkAssembler::with_limits(ReassemblyLimits {
            ttl: Duration::from_secs(1),
            max_groups: 0,
            max_bytes: 1024 * 1024,
            max_message_bytes: 1024 * 1024,
        });
        assert!(matches!(
            no_groups.add_from("sender", chunks[0].message.clone()),
            AssemblyOutcome::Rejected { error: Some(_) }
        ));
        assert_eq!(no_groups.groups_len(), 0);
    }

    #[test]
    fn chunk_ids_use_a_process_nonce_prefix() {
        let message = Message::new(MSG_BUSINESS_REQUEST);
        let first = chunk(&message, MAX_APP_PAYLOAD).expect("first chunk")[0].chunk_id;
        let second = chunk(&message, MAX_APP_PAYLOAD).expect("second chunk")[0].chunk_id;
        assert_ne!(first, second);
        assert_ne!(first >> 32, 0);
        assert_eq!(first >> 32, second >> 32);
        assert_ne!(
            chunk_id_prefix_for_pid(0x1234_5678, 100),
            chunk_id_prefix_for_pid(0x1234_5678, 101),
            "forked processes must derive different prefixes"
        );
    }

    #[test]
    fn inbound_processor_handles_empty_body_response() {
        let mut processor = InboundProcessor::new();
        let msg = build_business_response("admin", "test", "r1", "c1", b"", false);
        let result = processor.process("core", msg);
        // Non-chunked message should come through as Complete.
        match result.kind {
            InboundResultKind::Complete(m) => {
                assert!(m.body.is_empty());
            }
            _ => panic!("expected complete message"),
        }
    }
}
