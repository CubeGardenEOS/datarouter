use std::collections::VecDeque;
use std::os::raw::c_int;
use std::sync::{Condvar, Mutex};
use std::time::{Duration, Instant};

use super::options::normalize_timeout;

pub struct BoundedQueue<T> {
    capacity: usize,
    state: Mutex<State<T>>,
    available: Condvar,
}

struct State<T> {
    closed: bool,
    items: VecDeque<T>,
}

pub enum ReceiveResult<T> {
    Item(T),
    TooLarge { required: usize },
}

impl<T> BoundedQueue<T> {
    pub fn new(capacity: usize) -> Self {
        Self {
            capacity,
            state: Mutex::new(State {
                closed: false,
                items: VecDeque::with_capacity(capacity),
            }),
            available: Condvar::new(),
        }
    }

    pub fn try_push(&self, item: T) -> Result<(), T> {
        let Ok(mut state) = self.state.lock() else {
            return Err(item);
        };
        if state.closed || state.items.len() >= self.capacity {
            return Err(item);
        }
        state.items.push_back(item);
        self.available.notify_one();
        Ok(())
    }

    pub fn recv(&self, timeout_ms: u32) -> Result<T, c_int> {
        match self.recv_fitting(timeout_ms, |_| 0, usize::MAX)? {
            ReceiveResult::Item(item) => Ok(item),
            ReceiveResult::TooLarge { .. } => unreachable!("usize::MAX accepts every item"),
        }
    }

    /// Wait for the front item and remove it only when it fits. A too-large
    /// item remains at the head so a binding can resize and retry without
    /// losing message ordering.
    pub fn recv_fitting(
        &self,
        timeout_ms: u32,
        measure: impl Fn(&T) -> usize,
        capacity: usize,
    ) -> Result<ReceiveResult<T>, c_int> {
        let timeout = Duration::from_millis(normalize_timeout(
            timeout_ms,
            super::options::DEFAULT_RECV_TIMEOUT_MS,
        ) as u64);
        let deadline = Instant::now() + timeout;
        let mut state = self
            .state
            .lock()
            .map_err(|_| crate::DATAROUTER_ERR_INTERNAL)?;
        loop {
            if let Some(item) = state.items.front() {
                let required = measure(item);
                if required > capacity {
                    return Ok(ReceiveResult::TooLarge { required });
                }
                return state
                    .items
                    .pop_front()
                    .map(ReceiveResult::Item)
                    .ok_or(crate::DATAROUTER_ERR_INTERNAL);
            }
            if state.closed {
                return Err(crate::DATAROUTER_ERR_INTERNAL);
            }
            let now = Instant::now();
            if now >= deadline {
                return Err(crate::DATAROUTER_ERR_TIMEOUT);
            }
            let wait_for = deadline.saturating_duration_since(now);
            let (next_state, result) = self
                .available
                .wait_timeout(state, wait_for)
                .map_err(|_| crate::DATAROUTER_ERR_INTERNAL)?;
            state = next_state;
            if result.timed_out() {
                return Err(crate::DATAROUTER_ERR_TIMEOUT);
            }
        }
    }

    pub fn close(&self) {
        if let Ok(mut state) = self.state.lock() {
            state.closed = true;
            self.available.notify_all();
        }
    }

    pub fn len(&self) -> usize {
        self.state
            .lock()
            .map(|state| state.items.len())
            .unwrap_or(0)
    }

    pub fn is_empty(&self) -> bool {
        self.len() == 0
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn short_receive_keeps_the_front_item_for_retry() {
        let queue = BoundedQueue::new(2);
        queue.try_push(vec![1_u8, 2, 3, 4]).expect("queue item");

        let short = queue
            .recv_fitting(1, Vec::len, 2)
            .expect("inspect front item");
        assert!(matches!(short, ReceiveResult::TooLarge { required: 4 }));
        assert_eq!(queue.len(), 1);

        let received = queue.recv_fitting(1, Vec::len, 4).expect("retry receive");
        match received {
            ReceiveResult::Item(item) => assert_eq!(item, vec![1, 2, 3, 4]),
            ReceiveResult::TooLarge { .. } => panic!("resized buffer must fit"),
        }
        assert_eq!(queue.len(), 0);
    }
}
