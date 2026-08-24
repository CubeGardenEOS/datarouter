use std::collections::HashMap;
use std::os::raw::c_int;
use std::sync::atomic::Ordering;
use std::sync::{Mutex, mpsc};
use std::time::Duration;

use crate::protocol;

use super::FrontendInner;
use super::options::normalize_timeout;

pub struct AckWaiters {
    max_pending: usize,
    waiters: Mutex<HashMap<String, AckWaiter>>,
}

struct AckWaiter {
    expected_source: String,
    sender: mpsc::Sender<c_int>,
}

impl AckWaiters {
    pub fn new(max_pending: usize) -> Self {
        Self {
            max_pending,
            waiters: Mutex::new(HashMap::new()),
        }
    }

    pub fn register(
        &self,
        key: &str,
        expected_source: &str,
    ) -> Result<mpsc::Receiver<c_int>, c_int> {
        let (tx, rx) = mpsc::channel();
        let mut waiters = self
            .waiters
            .lock()
            .map_err(|_| crate::DATAROUTER_ERR_INTERNAL)?;
        if waiters.len() >= self.max_pending {
            return Err(crate::DATAROUTER_ERR_FULL);
        }
        if waiters.contains_key(key) {
            return Err(crate::DATAROUTER_ERR_BUSY);
        }
        waiters.insert(
            key.to_owned(),
            AckWaiter {
                expected_source: expected_source.to_owned(),
                sender: tx,
            },
        );
        Ok(rx)
    }

    pub fn dispatch(&self, key: &str, source: &str, code: c_int) {
        let Ok(mut waiters) = self.waiters.lock() else {
            return;
        };
        if waiters
            .get(key)
            .is_none_or(|waiter| waiter.expected_source != source)
        {
            return;
        }
        if let Some(waiter) = waiters.remove(key) {
            let _ = waiter.sender.send(code);
        }
    }

    pub fn remove(&self, key: &str) {
        if let Ok(mut waiters) = self.waiters.lock() {
            waiters.remove(key);
        }
    }

    pub fn close_all(&self, code: c_int) {
        if let Ok(mut waiters) = self.waiters.lock() {
            for (_, waiter) in waiters.drain() {
                let _ = waiter.sender.send(code);
            }
        }
    }

    pub fn len(&self) -> usize {
        self.waiters
            .lock()
            .map(|waiters| waiters.len())
            .unwrap_or(0)
    }

    pub fn is_empty(&self) -> bool {
        self.len() == 0
    }
}

pub fn send(
    inner: &FrontendInner,
    target_service: &str,
    packed: &[u8],
    timeout_ms: u32,
) -> Result<(), c_int> {
    if packed.is_empty() {
        return Err(crate::DATAROUTER_ERR_INVALID_ARGUMENT);
    }
    if packed.len() > inner.options.max_message_bytes {
        return Err(crate::DATAROUTER_ERR_PAYLOAD_TOO_LARGE);
    }
    if packed.len() <= protocol::MAX_APP_PAYLOAD {
        inner.send_frame(target_service, packed)?;
        inner.stats.sent_frames.fetch_add(1, Ordering::Relaxed);
        return Ok(());
    }
    send_chunked(inner, target_service, packed, timeout_ms)
}

fn send_chunked(
    inner: &FrontendInner,
    target_service: &str,
    packed: &[u8],
    timeout_ms: u32,
) -> Result<(), c_int> {
    let msg = protocol::unpack(packed).map_err(|_| crate::DATAROUTER_ERR_PROTOCOL)?;
    let chunks = protocol::chunk(&msg, protocol::MAX_APP_PAYLOAD)
        .map_err(|_| crate::DATAROUTER_ERR_PROTOCOL)?;
    tracing::debug!(
        target = %target_service,
        chunks = chunks.len(),
        "datarouter sending chunked message"
    );
    for chunk in chunks {
        send_one_chunk(inner, target_service, chunk.message, timeout_ms)?;
    }
    Ok(())
}

fn send_one_chunk(
    inner: &FrontendInner,
    target_service: &str,
    msg: protocol::Message,
    timeout_ms: u32,
) -> Result<(), c_int> {
    let ack_key = protocol::ack_key(&msg);
    let packed = protocol::pack(&msg).map_err(|_| crate::DATAROUTER_ERR_PROTOCOL)?;
    let waiter = match ack_key.as_ref() {
        Some(key) => Some(inner.ack_waiters.register(key, target_service)?),
        None => None,
    };
    if let Err(code) = inner.send_frame(target_service, &packed) {
        if let Some(key) = ack_key.as_ref() {
            inner.ack_waiters.remove(key);
        }
        return Err(code);
    }
    inner.stats.sent_frames.fetch_add(1, Ordering::Relaxed);
    inner.stats.sent_chunks.fetch_add(1, Ordering::Relaxed);
    if let (Some(key), Some(rx)) = (ack_key, waiter) {
        wait_for_ack(inner, &key, rx, timeout_ms)?;
    }
    Ok(())
}

fn wait_for_ack(
    inner: &FrontendInner,
    key: &str,
    rx: mpsc::Receiver<c_int>,
    timeout_ms: u32,
) -> Result<(), c_int> {
    let timeout =
        Duration::from_millis(normalize_timeout(timeout_ms, inner.options.ack_timeout_ms) as u64);
    match rx.recv_timeout(timeout) {
        Ok(crate::DATAROUTER_OK) => Ok(()),
        Ok(code) => Err(code),
        Err(mpsc::RecvTimeoutError::Timeout) => {
            tracing::warn!(key = %key, "datarouter ACK timeout");
            inner.ack_waiters.remove(key);
            inner.stats.ack_timeouts.fetch_add(1, Ordering::Relaxed);
            Err(crate::DATAROUTER_ERR_TIMEOUT)
        }
        Err(mpsc::RecvTimeoutError::Disconnected) => Err(crate::DATAROUTER_ERR_INTERNAL),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn ack_waiter_ignores_a_matching_id_from_the_wrong_source() {
        let waiters = AckWaiters::new(4);
        let rx = waiters
            .register("123:4", "expected-service")
            .expect("register waiter");

        waiters.dispatch("123:4", "forged-service", crate::DATAROUTER_OK);
        assert!(matches!(rx.try_recv(), Err(mpsc::TryRecvError::Empty)));
        assert_eq!(waiters.len(), 1);

        waiters.dispatch("123:4", "expected-service", crate::DATAROUTER_OK);
        assert_eq!(rx.recv().expect("expected ack"), crate::DATAROUTER_OK);
        assert_eq!(waiters.len(), 0);
    }
}
