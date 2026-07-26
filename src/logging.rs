use std::io::Write;
use std::sync::mpsc::{SyncSender, sync_channel};
use std::thread::{self, JoinHandle};

const MAX_EVENT_BYTES: usize = 64 * 1024;

enum LogCommand {
    Event(Vec<u8>),
    Shutdown,
}

pub struct BoundedLogWriter {
    sender: SyncSender<LogCommand>,
    buffer: Vec<u8>,
}

pub struct LogWorkerGuard {
    sender: SyncSender<LogCommand>,
    thread: Option<JoinHandle<()>>,
}

pub fn non_blocking_stderr(
    capacity: usize,
) -> (
    impl Fn() -> BoundedLogWriter + Clone + Send + Sync + 'static,
    LogWorkerGuard,
) {
    let (sender, receiver) = sync_channel::<LogCommand>(capacity.max(1));
    let thread = thread::Builder::new()
        .name("bizpilot-log-writer".to_string())
        .spawn(move || {
            let stderr = std::io::stderr();
            let mut stderr = stderr.lock();
            while let Ok(command) = receiver.recv() {
                match command {
                    LogCommand::Event(event) => {
                        let _ = stderr.write_all(&event);
                    }
                    LogCommand::Shutdown => break,
                }
            }
            let _ = stderr.flush();
        })
        .expect("failed to spawn bounded log writer");
    let guard = LogWorkerGuard {
        sender: sender.clone(),
        thread: Some(thread),
    };
    (
        move || BoundedLogWriter {
            sender: sender.clone(),
            buffer: Vec::with_capacity(512),
        },
        guard,
    )
}

impl Write for BoundedLogWriter {
    fn write(&mut self, bytes: &[u8]) -> std::io::Result<usize> {
        let available = MAX_EVENT_BYTES.saturating_sub(self.buffer.len());
        self.buffer
            .extend_from_slice(&bytes[..bytes.len().min(available)]);
        Ok(bytes.len())
    }

    fn flush(&mut self) -> std::io::Result<()> {
        Ok(())
    }
}

impl Drop for BoundedLogWriter {
    fn drop(&mut self) {
        if !self.buffer.is_empty() {
            let event = std::mem::take(&mut self.buffer);
            let _ = self.sender.try_send(LogCommand::Event(event));
        }
    }
}

impl Drop for LogWorkerGuard {
    fn drop(&mut self) {
        let _ = self.sender.send(LogCommand::Shutdown);
        if let Some(thread) = self.thread.take() {
            let _ = thread.join();
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn writer_truncates_events_and_never_blocks_on_a_full_queue() {
        let (sender, _receiver) = sync_channel(1);
        let make_writer = || BoundedLogWriter {
            sender: sender.clone(),
            buffer: Vec::new(),
        };
        let mut writer = make_writer();
        writer.write_all(&vec![b'x'; MAX_EVENT_BYTES * 2]).unwrap();
        assert_eq!(writer.buffer.len(), MAX_EVENT_BYTES);
        drop(writer);
        for _ in 0..100 {
            let mut writer = make_writer();
            writer.write_all(b"bounded\n").unwrap();
        }
    }
}
