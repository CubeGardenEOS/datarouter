//! End-to-end Frontend IPC benchmark: one-way throughput and pingpong latency
//! through the full Rust frontend + switch transport.
//!
//! Usage:
//! ```text
//! DATAROUTER_SWITCH_BIN=path/to/datarouter-switch cargo bench --bench ipc_frontend
//! ```
//!
//! The benchmark spawns its own switch on a unique shared-memory name, runs a
//! warm-up, then measures:
//! - one-way throughput: `frames` sends from portal drained by core via
//!   `recv_packed` (raw bytes, no repack);
//! - pingpong: portal -> core -> portal round trips via `recv_packed`;
//! - large-frame single-shot latency for payloads above the legacy 60 KiB cap.
//!
//! Reported numbers let old (unpack/repack reader, 60 KiB cap) and new
//! (verbatim raw-frame queue, 512 KiB single-frame) builds be compared on the
//! same machine.

use std::env;
use std::hint::black_box;
use std::process::{Child, Command};
use std::time::{Duration, Instant};

use blinkcms_datarouter::frontend::{Frontend, FrontendOptions};
use blinkcms_datarouter::protocol;

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

fn spawn_switch(name: &str) -> Child {
    let binary =
        env::var("DATAROUTER_SWITCH_BIN").unwrap_or_else(|_| "build/datarouter-switch".to_owned());
    Command::new(binary)
        .arg("--name")
        .arg(name)
        .arg("--ttl-ms")
        .arg("30000")
        .arg("--allow")
        .arg("core,portal")
        .stdout(std::process::Stdio::null())
        .stderr(std::process::Stdio::null())
        .spawn()
        .expect("spawn datarouter-switch")
}

fn connect(name: &str, service: &str) -> Frontend {
    let deadline = Instant::now() + Duration::from_secs(10);
    loop {
        let options = FrontendOptions {
            shm_name: name.to_owned(),
            service_name: service.to_owned(),
            connect_timeout_ms: 1000,
            recv_timeout_ms: 5000,
            reader_wait_ms: 10,
            ..FrontendOptions::default()
        };
        match Frontend::connect_routed(options) {
            Ok(frontend) => return frontend,
            Err(_) if Instant::now() < deadline => std::thread::sleep(Duration::from_millis(20)),
            Err(code) => panic!("connect {service} failed: {code}"),
        }
    }
}

fn pack_message(payload: &[u8]) -> Vec<u8> {
    let mut msg = protocol::Message::new(protocol::MSG_BUSINESS_REQUEST);
    msg.body = payload.to_vec();
    protocol::pack(&msg).expect("pack")
}

/// One-way throughput: N single-frame messages portal -> core, drained with
/// `recv_packed`. A sliding window keeps the 512 KiB rings full without
/// overflowing them (send returns ERR_FULL otherwise). `recv_packed` hands the
/// application the raw wire bytes, so the measured path is the reader's
/// receive + queue + app copy.
fn one_way(core: &Frontend, portal: &Frontend, payload: &[u8], frames: usize) -> Duration {
    let packed = pack_message(payload);
    let window = ((256 * 1024) / payload.len().max(1)).clamp(1, 64);
    let mut buf = vec![0u8; 1 << 20];
    let started = Instant::now();
    let mut sent = 0usize;
    let mut received = 0usize;
    while received < frames {
        if sent < frames && sent - received < window {
            match portal.send("core", &packed, 5000) {
                Ok(()) => sent += 1,
                Err(blinkcms_datarouter::DATAROUTER_ERR_FULL) => {
                    std::thread::sleep(Duration::from_micros(100));
                }
                Err(code) => panic!("send failed: {code}"),
            }
            continue;
        }
        let info = core.recv_packed(&mut buf, 5000).expect("recv");
        if info.len > 0 {
            received += 1;
        }
    }
    started.elapsed()
}

fn pingpong(
    core: &Frontend,
    portal: &Frontend,
    payload: &[u8],
    round_trips: usize,
) -> (Duration, Duration, Duration) {
    let packed = pack_message(payload);
    let mut core_buf = vec![0u8; 1 << 20];
    let mut portal_buf = vec![0u8; 1 << 20];
    let mut samples = Vec::with_capacity(round_trips);
    for _ in 0..round_trips {
        let started = Instant::now();
        portal.send("core", &packed, 5000).expect("send");
        let info = core.recv_packed(&mut core_buf, 5000).expect("recv");
        assert!(info.len > 0);
        core.send("portal", &packed, 5000).expect("reply");
        let info = portal.recv_packed(&mut portal_buf, 5000).expect("recv");
        assert!(info.len > 0);
        samples.push(started.elapsed());
    }
    samples.sort_unstable();
    let p50 = samples[round_trips / 2];
    let p99 = samples[(round_trips * 99) / 100];
    let total: Duration = samples.iter().sum();
    (total, p50, p99)
}

fn main() {
    let frames = env::var("IPC_BENCH_FRAMES")
        .ok()
        .and_then(|value| value.parse().ok())
        .unwrap_or(4000);
    let round_trips = env::var("IPC_BENCH_ROUND_TRIPS")
        .ok()
        .and_then(|value| value.parse().ok())
        .unwrap_or(2000);

    let name = shm_name("frontend");
    cleanup(&name);
    let mut child = spawn_switch(&name);
    let core = connect(&name, "core");
    let portal = connect(&name, "portal");

    // Warm-up.
    let warmup = vec![0x11u8; 1024];
    let _ = one_way(&core, &portal, &warmup, 100);

    println!("== one-way throughput (portal -> core, recv_packed) ==");
    for (label, payload) in [
        ("1KiB", vec![0x22u8; 1 * 1024]),
        ("32KiB", vec![0x33u8; 32 * 1024]),
        ("64KiB", vec![0x44u8; 64 * 1024]),
        ("300KiB", vec![0x55u8; 300 * 1024]),
    ] {
        let n = if payload.len() >= 300 * 1024 {
            500
        } else {
            frames
        };
        let elapsed = one_way(&core, &portal, &payload, n);
        let per_frame = elapsed / n as u32;
        let msg_s = n as f64 / elapsed.as_secs_f64();
        let mb_s = (payload.len() as f64) * msg_s / (1024.0 * 1024.0);
        println!(
            "{label:>8}: {n} frames in {elapsed:?} ({per_frame:?}/frame, {msg_s:.0} msg/s, {mb_s:.1} MiB/s)"
        );
    }

    println!("== pingpong latency (portal -> core -> portal) ==");
    for (label, payload) in [
        ("64B", vec![0x66u8; 64]),
        ("1KiB", vec![0x67u8; 1 * 1024]),
        ("300KiB", vec![0x68u8; 300 * 1024]),
    ] {
        let rt = if payload.len() >= 300 * 1024 {
            100
        } else {
            round_trips
        };
        let (total, p50, p99) = pingpong(&core, &portal, &payload, rt);
        let avg = total / rt as u32;
        let rt_s = rt as f64 / total.as_secs_f64();
        println!(
            "{label:>8}: {rt} round trips, avg {avg:?} p50 {p50:?} p99 {p99:?} ({rt_s:.0} rt/s)"
        );
    }

    let _ = child.kill();
    let _ = child.wait();
    cleanup(&name);
    black_box(());
}
