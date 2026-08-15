//! Two-process direct (unsafe) plane regression tests.
//!
//! The benchmark suite found two direct-plane failure modes that only appear
//! with real cross-process peers (in-process tests never trigger them):
//!
//! 1. **False early timeout in `datarouter_wait`**: a long receive timeout
//!    (e.g. 5000ms) returned after a single ~100ms signal-wait cycle, so a
//!    receiver whose peer started slowly bailed, unregistered, and tore down
//!    its slot (which then collided with a later registration).
//! 2. **Peer mid-registration NOT_FOUND**: a sender that starts while the
//!    target's registry entry is not yet ACTIVE surfaced a phantom `NOT_FOUND`
//!    (and bindings that treat a burst of such failures as "sent" lost
//!    frames).
//!
//! The transport now (a) continues waiting for the full timeout budget in
//! `datarouter_wait` and (b) bounds transient-send retries
//! (`SEND_TRANSIENT_RETRIES`) on both data planes, while `ERR_FULL` keeps its
//! backpressure semantics. These tests run the real two-process layout
//! (switch + portal + core) and assert zero frame loss / zero phantom errors
//! with **no application-side warm-up or retry**.
//!
//! Run: `cargo test --features unsafe --test direct_plane_multiprocess`

#![cfg(feature = "unsafe")]

use std::env;
use std::process::{Command, Stdio};
use std::time::{Duration, Instant};

use blinkcms_datarouter::frontend::{Frontend, FrontendOptions};
use blinkcms_datarouter::protocol::{self, Message, MSG_BUSINESS_REQUEST, MSG_BUSINESS_RESPONSE};
use blinkcms_datarouter::switch_frontend::{LogLevel, SwitchConfig, SwitchHandle};
use blinkcms_datarouter::DATAROUTER_ERR_FULL;

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

fn connect(shm: &str, service: &str) -> Frontend {
    let deadline = Instant::now() + Duration::from_secs(15);
    loop {
        let options = FrontendOptions {
            shm_name: shm.to_owned(),
            service_name: service.to_owned(),
            connect_timeout_ms: 2000,
            recv_timeout_ms: 5000,
            reader_wait_ms: 10,
            ..Default::default()
        };
        match Frontend::connect(options) {
            Ok(f) => return f,
            Err(_) if Instant::now() < deadline => std::thread::sleep(Duration::from_millis(20)),
            Err(code) => panic!("connect {service} failed: {code}"),
        }
    }
}

fn pack(body: &[u8], typ: u8) -> Vec<u8> {
    let mut m = Message::new(typ);
    m.body = body.to_vec();
    protocol::pack(&m).expect("pack")
}

fn child_sender(shm: &str, frames: usize, window: usize, payload: usize, stagger: bool) -> i32 {
    if stagger {
        // Sender starts later than the receiver and sends immediately after
        // connect with NO warm-up; the transport must absorb the window where
        // the peer's entry is not yet visible instead of erroring.
        std::thread::sleep(Duration::from_millis(700));
    }
    let f = connect(shm, "portal");
    let mut buf = vec![0u8; 1 << 20];
    let frame = pack(&vec![0x33u8; payload], MSG_BUSINESS_REQUEST);
    let mut sent = 0usize;
    let mut acked = 0usize;
    let mut errors = 0u64;
    let mut stall = 0u32;
    while acked < frames && stall < 3 {
        while sent < frames && sent - acked < window {
            match f.send("core", &frame, 5000) {
                Ok(()) => sent += 1,
                Err(DATAROUTER_ERR_FULL) => {
                    std::thread::sleep(Duration::from_micros(100));
                    break;
                }
                Err(code) => {
                    eprintln!("sender send error code={code}");
                    errors += 1;
                    sent += 1;
                }
            }
        }
        match f.recv_packed(&mut buf, 100) {
            Ok(info) if info.len > 0 => {
                acked += 1;
                stall = 0;
            }
            _ => {
                errors += 1;
                stall += 1;
            }
        }
    }
    eprintln!("sender: sent={sent} acked={acked} errors={errors} (frames={frames})");
    if errors > 0 || acked != frames {
        1
    } else {
        0
    }
}

fn child_receiver(shm: &str, frames: usize) -> i32 {
    let f = connect(shm, "core");
    let mut buf = vec![0u8; 1 << 20];
    let ack = pack(&[], MSG_BUSINESS_RESPONSE);
    let mut received = 0usize;
    let mut errors = 0u64;
    let mut stall = 0u32;
    while received < frames && stall < 3 {
        // First recv may legitimately wait for a staggered sender: use the
        // full timeout once, then short polls so real stalls are detected.
        // A correct `datarouter_wait` keeps waiting for the full 5s budget.
        let timeout = if received == 0 { 5000 } else { 100 };
        match f.recv_packed(&mut buf, timeout) {
            Ok(info) if info.len > 0 => {
                received += 1;
                stall = 0;
                if f.send("portal", &ack, 5000).is_err() {
                    errors += 1;
                }
            }
            _ => {
                errors += 1;
                stall += 1;
            }
        }
    }
    eprintln!("receiver: received={received} errors={errors}");
    if errors > 0 || received != frames {
        1
    } else {
        0
    }
}

/// Spawn the switch + two child roles, wait for both, and assert PASS.
fn run_pair(name: &str, frames: usize, window: usize, payload: usize, stagger: bool) {
    let mut sw = SwitchHandle::start(SwitchConfig {
        shm_name: name.to_owned(),
        allow: vec!["core".to_owned(), "portal".to_owned()],
        ttl_ms: 30000,
        log_level: LogLevel::Error,
        ..Default::default()
    })
    .expect("start switch");

    let exe = env::current_exe().expect("current exe");
    let spawn = |role: &str, extra: Option<usize>| {
        let mut cmd = Command::new(&exe);
        cmd.env("DATAROUTER_MP_CHILD", role)
            .args([name, &frames.to_string(), &window.to_string(), &payload.to_string()]);
        if let Some(v) = extra {
            cmd.arg(v.to_string());
        }
        cmd.stdout(Stdio::null())
            .stderr(Stdio::inherit())
            .spawn()
            .expect("spawn child")
    };
    let mut core = spawn("receiver", None);
    if stagger {
        std::thread::sleep(Duration::from_millis(700));
    }
    let mut portal = spawn("sender", Some(stagger as usize));
    let core_rc = core.wait().expect("wait core");
    let portal_rc = portal.wait().expect("wait portal");
    assert!(
        core_rc.success(),
        "{name}: receiver role failed (rc={core_rc})"
    );
    assert!(
        portal_rc.success(),
        "{name}: sender role failed (rc={portal_rc})"
    );
    let _ = sw.stop();
    cleanup(name);
}

fn main() {
    // Child dispatch: the parent spawns this binary with DATAROUTER_MP_CHILD
    // set to run one side of a two-process pair.
    if let Ok(role) = env::var("DATAROUTER_MP_CHILD") {
        let args: Vec<String> = env::args().collect();
        let shm = args[1].clone();
        let frames: usize = args[2].parse().expect("frames");
        let window: usize = args[3].parse().expect("window");
        let payload: usize = args[4].parse().expect("payload");
        let stagger = args.get(5).map(|s| s == "1").unwrap_or(false);
        let rc = if role == "sender" {
            child_sender(&shm, frames, window, payload, stagger)
        } else {
            child_receiver(&shm, frames)
        };
        std::process::exit(rc);
    }

    // Test runner (harness = false): run both scenarios sequentially.
    let name = shm_name("mp-ow");
    run_pair(&name, 2000, 32, 32768, false);
    println!("direct_plane_windowed_oneway_no_loss: PASS");

    let name = shm_name("mp-st");
    run_pair(&name, 300, 32, 1024, true);
    println!("direct_plane_staggered_start_no_phantom_errors: PASS");
}
