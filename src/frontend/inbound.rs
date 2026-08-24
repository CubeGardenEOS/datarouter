use std::os::raw::c_char;
use std::panic::{self, AssertUnwindSafe};
use std::sync::Arc;
use std::sync::atomic::Ordering;
use std::thread::{self, JoinHandle};
use std::time::{Duration, Instant};

use crate::protocol::{self, InboundResultKind};

use super::{FrontendInner, QueuedMessage};

const DRAIN_FRAME_BUDGET: usize = 256;

pub fn spawn_reader(inner: Arc<FrontendInner>) -> JoinHandle<()> {
    thread::spawn(move || {
        inner.stats.reader_alive.store(1, Ordering::Relaxed);
        let result = panic::catch_unwind(AssertUnwindSafe(|| run_receive_loop(Arc::clone(&inner))));
        inner.stats.reader_alive.store(0, Ordering::Relaxed);
        if result.is_err() {
            inner.stats.reader_errors.fetch_add(1, Ordering::Relaxed);
            eprintln!(
                "datarouter frontend reader panic service={} shm={}",
                inner.options.service_name, inner.options.shm_name
            );
        } else if !inner.closing.load(Ordering::Acquire) {
            eprintln!(
                "datarouter frontend reader exited service={} shm={}",
                inner.options.service_name, inner.options.shm_name
            );
        }
    })
}

fn run_receive_loop(inner: Arc<FrontendInner>) {
    let mut processor = protocol::InboundProcessor::with_limits(protocol::ReassemblyLimits {
        ttl: Duration::from_millis(u64::from(inner.options.chunk_ttl_ms)),
        max_groups: inner.options.max_reassembly_groups,
        max_bytes: inner.options.max_reassembly_bytes,
        max_message_bytes: inner.options.max_message_bytes,
    });
    while !inner.closing.load(Ordering::Acquire) {
        expire_reassemblies(&inner, &mut processor);
        if !wait_for_frame(&inner) {
            continue;
        }
        drain_available(&inner, &mut processor);
    }
}

fn wait_for_frame(inner: &FrontendInner) -> bool {
    let expiry_poll_ms = (inner.options.chunk_ttl_ms / 2).max(1);
    let wait_ms = inner.options.reader_wait_ms.min(expiry_poll_ms);
    let rc = inner
        .with_port(|port| port.wait(wait_ms))
        .unwrap_or(crate::DATAROUTER_ERR_INTERNAL);
    match rc {
        crate::DATAROUTER_OK => true,
        crate::DATAROUTER_ERR_TIMEOUT | crate::DATAROUTER_ERR_AGAIN => false,
        crate::DATAROUTER_ERR_NOT_FOUND => {
            let _ = inner.recover_self_after_not_found("reader wait");
            false
        }
        code => {
            inner.stats.reader_errors.fetch_add(1, Ordering::Relaxed);
            eprintln!(
                "datarouter frontend reader wait failed service={} shm={} code={}",
                inner.options.service_name, inner.options.shm_name, code
            );
            false
        }
    }
}

fn drain_available(inner: &FrontendInner, processor: &mut protocol::InboundProcessor) {
    for _ in 0..DRAIN_FRAME_BUDGET {
        let mut buffer = inner.take_buffer();
        let frame = inner
            .with_port(|port| port.recv_into(&mut buffer))
            .unwrap_or(Err(crate::DATAROUTER_ERR_INTERNAL));
        match frame {
            Ok(Some(frame)) => {
                inner.stats.received_frames.fetch_add(1, Ordering::Relaxed);
                handle_raw_frame(inner, processor, frame.source, frame.data);
            }
            Ok(None) => {
                inner.recycle_buffer(buffer);
                return;
            }
            Err(crate::DATAROUTER_ERR_NOT_FOUND) => {
                inner.stats.dropped_frames.fetch_add(1, Ordering::Relaxed);
                inner.recycle_buffer(buffer);
                let _ = inner.recover_self_after_not_found("reader recv");
                return;
            }
            Err(code) => {
                inner.stats.dropped_frames.fetch_add(1, Ordering::Relaxed);
                inner.stats.reader_errors.fetch_add(1, Ordering::Relaxed);
                inner.recycle_buffer(buffer);
                eprintln!(
                    "datarouter frontend reader recv failed service={} shm={} code={}",
                    inner.options.service_name, inner.options.shm_name, code
                );
                return;
            }
        }
    }
}

/// Route one raw wire frame with the minimum possible copies.
///
/// A well-formed single-frame message (no chunk metadata TLV, not a chunk
/// control message) is queued verbatim: the bytes received from the ring are
/// handed to the application queue without unpacking or repacking. Chunked and
/// control frames fall back to the full protocol path, whose behavior is
/// identical to the historical reader.
fn handle_raw_frame(
    inner: &FrontendInner,
    processor: &mut protocol::InboundProcessor,
    source: [c_char; crate::DATAROUTER_SERVICE_NAME_MAX + 1],
    raw: Vec<u8>,
) {
    let Some((message_type, _, _vd_len, _, chunk)) = protocol::classify_frame(&raw) else {
        inner.stats.protocol_errors.fetch_add(1, Ordering::Relaxed);
        return;
    };
    let is_chunk_control = matches!(
        message_type,
        protocol::MSG_CHUNK_ACK | protocol::MSG_CHUNK_ERROR
    );
    if !is_chunk_control && chunk == protocol::ChunkClass::None {
        queue_complete_raw(inner, source, raw);
        return;
    }
    match protocol::unpack(&raw) {
        Ok(msg) => handle_inbound_message(inner, processor, source, msg),
        Err(_) => {
            inner.stats.protocol_errors.fetch_add(1, Ordering::Relaxed);
        }
    }
}

/// Queue a complete single-frame message without any repacking copy.
/// Matches the historical `queue_complete` for the no-chunk-ack case.
fn queue_complete_raw(
    inner: &FrontendInner,
    source: [c_char; crate::DATAROUTER_SERVICE_NAME_MAX + 1],
    raw: Vec<u8>,
) {
    if raw.len() > inner.options.max_message_bytes {
        inner.stats.dropped_frames.fetch_add(1, Ordering::Relaxed);
        return;
    }
    tracing::debug!(len = raw.len(), "datarouter inbound frame queued");
    match inner.complete_queue.try_push(QueuedMessage {
        source,
        packed: raw,
    }) {
        Ok(()) => {
            inner
                .stats
                .received_complete
                .fetch_add(1, Ordering::Relaxed);
        }
        Err(queued) => {
            inner.stats.queue_full.fetch_add(1, Ordering::Relaxed);
            let _ = queued;
        }
    }
}

fn handle_inbound_message(
    inner: &FrontendInner,
    processor: &mut protocol::InboundProcessor,
    source: [c_char; crate::DATAROUTER_SERVICE_NAME_MAX + 1],
    msg: protocol::Message,
) {
    let source_name = super::c_source_to_string(&source);
    let result = processor.process(&source_name, msg);
    if let Some(error) = result.error_message.as_ref() {
        inner
            .stats
            .reassembly_rejected
            .fetch_add(1, Ordering::Relaxed);
        inner.stats.protocol_errors.fetch_add(1, Ordering::Relaxed);
        send_control(inner, &source, error);
    }
    if let Some(ack) = result.ack_message.as_ref() {
        send_control(inner, &source, ack);
    }
    match result.kind {
        InboundResultKind::ChunkAck(msg) => {
            inner
                .stats
                .received_chunk_acks
                .fetch_add(1, Ordering::Relaxed);
            dispatch_ack(inner, &source_name, msg, crate::DATAROUTER_OK);
        }
        InboundResultKind::ChunkError(msg) => {
            inner
                .stats
                .received_chunk_errors
                .fetch_add(1, Ordering::Relaxed);
            dispatch_ack(inner, &source_name, msg, crate::DATAROUTER_ERR_PROTOCOL);
        }
        InboundResultKind::Complete(msg) => {
            queue_complete(inner, source, msg, result.final_ack_message.as_ref());
        }
        InboundResultKind::Pending => {}
        InboundResultKind::Rejected => {}
    }
}

fn expire_reassemblies(inner: &FrontendInner, processor: &mut protocol::InboundProcessor) {
    for expired in processor.expire(Instant::now()) {
        tracing::warn!(source = %expired.source, "datarouter reassembly expired");
        inner
            .stats
            .reassembly_expired
            .fetch_add(1, Ordering::Relaxed);
        let Some(source) = source_from_string(&expired.source) else {
            inner.stats.protocol_errors.fetch_add(1, Ordering::Relaxed);
            continue;
        };
        send_control(inner, &source, &expired.error);
    }
}

fn source_from_string(source: &str) -> Option<[c_char; crate::DATAROUTER_SERVICE_NAME_MAX + 1]> {
    let bytes = source.as_bytes();
    if bytes.is_empty() || bytes.len() > crate::DATAROUTER_SERVICE_NAME_MAX || bytes.contains(&0) {
        return None;
    }
    let mut result = [0; crate::DATAROUTER_SERVICE_NAME_MAX + 1];
    for (target, value) in result.iter_mut().zip(bytes) {
        *target = *value as c_char;
    }
    Some(result)
}

fn send_control(
    inner: &FrontendInner,
    source: &[c_char; crate::DATAROUTER_SERVICE_NAME_MAX + 1],
    msg: &protocol::Message,
) {
    if let Ok(packed) = protocol::pack(msg) {
        let _ = inner.send_frame_to_source(source, &packed);
    }
}

fn dispatch_ack(inner: &FrontendInner, source: &str, msg: protocol::Message, code: i32) {
    let Some(key) = protocol::ack_key(&msg) else {
        return;
    };
    inner.ack_waiters.dispatch(&key, source, code);
}

fn queue_complete(
    inner: &FrontendInner,
    source: [c_char; crate::DATAROUTER_SERVICE_NAME_MAX + 1],
    msg: protocol::Message,
    ack_for_last_chunk: Option<&protocol::Message>,
) {
    let Ok(packed) = protocol::pack(&msg) else {
        inner.stats.protocol_errors.fetch_add(1, Ordering::Relaxed);
        send_chunk_error(inner, &source, ack_for_last_chunk);
        return;
    };
    if packed.len() > inner.options.max_message_bytes {
        inner.stats.dropped_frames.fetch_add(1, Ordering::Relaxed);
        send_chunk_error(inner, &source, ack_for_last_chunk);
        return;
    }
    match inner
        .complete_queue
        .try_push(QueuedMessage { source, packed })
    {
        Ok(()) => {
            inner
                .stats
                .received_complete
                .fetch_add(1, Ordering::Relaxed);
            if let Some(ack) = ack_for_last_chunk {
                send_control(inner, &source, ack);
            }
        }
        Err(queued) => {
            inner.stats.queue_full.fetch_add(1, Ordering::Relaxed);
            send_chunk_error(inner, &queued.source, ack_for_last_chunk);
        }
    }
}

fn send_chunk_error(
    inner: &FrontendInner,
    source: &[c_char; crate::DATAROUTER_SERVICE_NAME_MAX + 1],
    ack_for_last_chunk: Option<&protocol::Message>,
) {
    let Some(ack) = ack_for_last_chunk else {
        return;
    };
    let msg = protocol::Message {
        message_type: protocol::MSG_CHUNK_ERROR,
        flags: 0,
        var_data: ack.var_data.clone(),
        body: Vec::new(),
    };
    send_control(inner, source, &msg);
}
