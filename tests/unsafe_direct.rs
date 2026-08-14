//! Direct (unsafe) data-plane integration tests.
//!
//! These tests exercise the `unsafe` Cargo feature (development-only, off by
//! default): `Frontend::connect_unsafe` opens a port through the switch's
//! control plane but moves data with `unsafe_send`/`unsafe_recv`, which write
//! and read the target RX ring directly (2 payload copies per direction) and
//! never route payload bytes through the switch.

#![cfg(feature = "unsafe")]

use std::time::Duration;

use blinkcms_datarouter::frontend::{Frontend, FrontendOptions};
use blinkcms_datarouter::protocol::{self, MSG_BUSINESS_REQUEST};
use blinkcms_datarouter::switch_frontend::{LogLevel, SwitchConfig, SwitchHandle};
use blinkcms_datarouter::{DATAROUTER_ERR_FULL, DATAROUTER_ERR_NOT_FOUND};

fn shm_name(prefix: &str) -> String {
    let nanos = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .expect("system time")
        .as_nanos();
    format!("/datarouter-{prefix}-{}-{nanos}", std::process::id())
}

fn cleanup(name: &str) {
    let _ = std::fs::remove_file(format!("/dev/shm/{}", name.trim_start_matches('/')));
}

struct TestSwitch {
    handle: SwitchHandle,
    name: String,
}

fn start_switch(prefix: &str) -> TestSwitch {
    let name = shm_name(prefix);
    cleanup(&name);
    let handle = SwitchHandle::start(SwitchConfig {
        shm_name: name.clone(),
        ttl_ms: 30000,
        allow: vec!["core".to_owned(), "portal".to_owned()],
        log_level: LogLevel::Error,
        ready_timeout: Duration::from_secs(10),
        ..SwitchConfig::default()
    })
    .expect("start embedded switch");
    TestSwitch { handle, name }
}

impl Drop for TestSwitch {
    fn drop(&mut self) {
        let _ = self.handle.stop();
        cleanup(&self.name);
    }
}

fn connect_unsafe(sw: &TestSwitch, service: &str) -> Frontend {
    Frontend::connect_unsafe(FrontendOptions {
        shm_name: sw.name.clone(),
        service_name: service.to_owned(),
        connect_timeout_ms: 1000,
        recv_timeout_ms: 2000,
        reader_wait_ms: 10,
        ..FrontendOptions::default()
    })
    .expect("connect_unsafe")
}

fn connect_normal(sw: &TestSwitch, service: &str) -> Frontend {
    Frontend::connect(FrontendOptions {
        shm_name: sw.name.clone(),
        service_name: service.to_owned(),
        connect_timeout_ms: 1000,
        recv_timeout_ms: 2000,
        reader_wait_ms: 10,
        ..FrontendOptions::default()
    })
    .expect("connect")
}

fn pack_request(body: &[u8]) -> Vec<u8> {
    let mut msg = protocol::Message::new(MSG_BUSINESS_REQUEST);
    msg.body = body.to_vec();
    protocol::pack(&msg).expect("pack")
}

fn c_source(source: &[i8; 32]) -> String {
    source
        .iter()
        .take_while(|value| **value != 0)
        .map(|value| *value as u8 as char)
        .collect()
}

#[test]
fn unsafe_round_trip_delivers_raw_bytes() {
    let sw = start_switch("unsafe-rt");
    let core = connect_unsafe(&sw, "core");
    let portal = connect_unsafe(&sw, "portal");

    let body = b"direct data-plane payload".to_vec();
    let packed = pack_request(&body);
    portal
        .unsafe_send("core", &packed)
        .expect("unsafe send succeeds");

    let mut buf = vec![0u8; 1 << 20];
    let info = core
        .unsafe_recv(&mut buf, 2000)
        .expect("unsafe recv ok")
        .expect("frame arrives");
    assert_eq!(info.len, packed.len());
    assert_eq!(&buf[..info.len], &packed[..], "raw wire bytes match");
    assert_eq!(c_source(&info.source), "portal", "source resolves to portal");

    let decoded = protocol::unpack(&buf[..info.len]).expect("packed frame is well-formed");
    assert_eq!(decoded.body, body);
}

#[test]
fn unsafe_send_reaches_normal_reader_and_normal_send_reaches_unsafe_reader() {
    let sw = start_switch("unsafe-mixed");
    let core = connect_normal(&sw, "core");
    let portal = connect_unsafe(&sw, "portal");

    // Direct sender -> normal reader (reader thread + queue).
    let packed = pack_request(b"direct-to-normal");
    portal
        .unsafe_send("core", &packed)
        .expect("unsafe send to normal reader");
    let received = core.recv(1000).expect("normal reader receives");
    assert_eq!(received.source, "portal");
    assert_eq!(received.message.body, b"direct-to-normal");

    // Normal sender (switch routed) -> direct reader.
    let reply = pack_request(b"normal-to-direct");
    core.send("portal", &reply, 1000).expect("normal send");
    let mut buf = vec![0u8; 1 << 20];
    let info = portal
        .unsafe_recv(&mut buf, 2000)
        .expect("unsafe recv ok")
        .expect("frame arrives");
    assert_eq!(&buf[..info.len], &reply[..]);
    assert_eq!(c_source(&info.source), "core");
}

#[test]
fn unsafe_send_returns_full_when_target_ring_is_full() {
    let sw = start_switch("unsafe-full");
    let core = connect_unsafe(&sw, "core");
    let portal = connect_unsafe(&sw, "portal");

    let packed = pack_request(&vec![0xABu8; 64 * 1024]);
    let mut saw_full = false;
    for _ in 0..64 {
        match portal.unsafe_send("core", &packed) {
            Ok(()) => {}
            Err(DATAROUTER_ERR_FULL) => {
                saw_full = true;
                break;
            }
            Err(code) => panic!("unexpected unsafe send error: {code}"),
        }
    }
    assert!(saw_full, "a full 512 KiB RX ring must reject with ERR_FULL");

    // Drain one frame, then the same send succeeds again.
    let mut buf = vec![0u8; 1 << 20];
    let info = core
        .unsafe_recv(&mut buf, 2000)
        .expect("recv ok")
        .expect("frame arrives");
    assert_eq!(info.len, packed.len());
    portal
        .unsafe_send("core", &packed)
        .expect("send succeeds after drain");
}

#[test]
fn unsafe_send_reports_not_found_for_missing_target() {
    let sw = start_switch("unsafe-nf");
    let portal = connect_unsafe(&sw, "portal");
    let packed = pack_request(b"hello");
    match portal.unsafe_send("ghost", &packed) {
        Err(DATAROUTER_ERR_NOT_FOUND) => {}
        other => panic!("expected NOT_FOUND for unregistered target, got {other:?}"),
    }
}

#[test]
fn concurrent_unsafe_recv_never_torn_reads() {
    // The direct plane has no reader thread; several application threads may
    // call unsafe_recv on the same frontend. The per-port RX mutex must keep
    // each frame whole and hand each thread exactly one frame.
    let sw = start_switch("unsafe-multirecv");
    let core = connect_unsafe(&sw, "core");
    let portal = connect_unsafe(&sw, "portal");

    const FRAMES: usize = 64;
    let frames: Vec<Vec<u8>> = (0..FRAMES)
        .map(|i| pack_request(&format!("concurrent-frame-{i}").into_bytes()))
        .collect();

    let core = std::sync::Arc::new(core);
    let mut threads = Vec::new();
    for _ in 0..4 {
        let core = std::sync::Arc::clone(&core);
        threads.push(std::thread::spawn(move || {
            let mut buf = vec![0u8; 1 << 20];
            let mut got = 0usize;
            let mut body_ok = true;
            loop {
                let info = match core.unsafe_recv(&mut buf, 5000) {
                    Ok(Some(info)) => info,
                    Ok(None) => break,
                    Err(code) => panic!("unsafe_recv failed: {code}"),
                };
                if info.len == 0 || info.len > buf.len() {
                    body_ok = false;
                    break;
                }
                if let Ok(msg) = protocol::unpack(&buf[..info.len])
                    && !msg.body.starts_with(b"concurrent-frame-")
                {
                    body_ok = false;
                    break;
                }
                got += 1;
                if got >= FRAMES {
                    break;
                }
            }
            (got, body_ok)
        }));
    }
    for packed in &frames {
        portal
            .unsafe_send("core", packed)
            .expect("unsafe send succeeds");
    }
    let mut total = 0usize;
    for thread in threads {
        let (got, body_ok) = thread.join().expect("recv thread");
        assert!(body_ok, "torn or corrupted frame body observed");
        total += got;
    }
    assert_eq!(total, FRAMES, "every frame delivered exactly once");
}

#[test]
fn unsafe_recv_oversized_frame_is_retained_for_a_larger_buffer() {
    let sw = start_switch("unsafe-oversize");
    let core = connect_unsafe(&sw, "core");
    let portal = connect_unsafe(&sw, "portal");

    let packed = pack_request(&vec![0xC3u8; 300 * 1024]);
    portal.unsafe_send("core", &packed).expect("unsafe send");

    let mut small = vec![0u8; 1024];
    let rc = core.unsafe_recv(&mut small, 2000);
    assert!(
        matches!(rc, Err(blinkcms_datarouter::DATAROUTER_ERR_PAYLOAD_TOO_LARGE)),
        "small buffer must reject with PAYLOAD_TOO_LARGE, got {rc:?}"
    );

    // The rejected frame must NOT have been consumed: a fresh large buffer
    // still receives the same wire bytes.
    let mut large = vec![0u8; 1 << 20];
    let info = core
        .unsafe_recv(&mut large, 2000)
        .expect("unsafe recv ok")
        .expect("frame still present");
    assert_eq!(info.len, packed.len());
    assert_eq!(&large[..info.len], &packed[..]);
}
