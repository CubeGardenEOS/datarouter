#![cfg(feature = "unsafe")]
use blinkcms_datarouter::frontend::{Frontend, FrontendOptions};
use blinkcms_datarouter::protocol::{self, Message, MSG_BUSINESS_REQUEST};
use blinkcms_datarouter::switch_frontend::{SwitchConfig, SwitchHandle};
use blinkcms_datarouter::{DATAROUTER_ERR_AGAIN, DATAROUTER_ERR_FULL};
use std::sync::{Arc, Barrier};
use std::time::{Duration, Instant};

#[test]
fn concurrent_direct_and_routed_writers_preserve_every_frame() {
    let name = format!("/dr-mixed-writers-{}", std::process::id());
    let mut switch = SwitchHandle::start(SwitchConfig {
        shm_name: name.clone(), max_ports: 8, ttl_ms: 60000,
        allow: ["core", "sender0", "sender1", "sender2", "sender3"].map(str::to_owned).to_vec(),
        ..SwitchConfig::default()
    }).unwrap();
    let options = |service: String| FrontendOptions {
        shm_name: name.clone(), service_name: service, ..FrontendOptions::default()
    };
    let receiver = Frontend::connect_unsafe(options("core".into())).unwrap();
    let barrier = Arc::new(Barrier::new(5));
    std::thread::scope(|scope| {
        for sender in 0u32..4 {
            let options = options(format!("sender{sender}"));
            let barrier = barrier.clone();
            scope.spawn(move || {
                let port = if sender < 2 { Frontend::connect_unsafe(options) }
                           else { Frontend::connect_routed(options) }.unwrap();
                barrier.wait();
                let deadline = Instant::now() + Duration::from_secs(30);
                for seq in 0u32..1000 {
                    let mut message = Message::new(MSG_BUSINESS_REQUEST);
                    message.body = vec![sender as u8; 16384];
                    message.body[..4].copy_from_slice(&sender.to_le_bytes());
                    message.body[4..8].copy_from_slice(&seq.to_le_bytes());
                    let packed = protocol::pack(&message).unwrap();
                    loop {
                        let result = if sender < 2 { port.unsafe_send("core", &packed) }
                                     else { port.send("core", &packed, 1000) };
                        match result {
                            Ok(()) => break,
                            Err(DATAROUTER_ERR_AGAIN | DATAROUTER_ERR_FULL) => {
                                assert!(Instant::now() < deadline, "writer stalled");
                                std::thread::yield_now();
                            }
                            Err(code) => panic!("writer failed: {code}"),
                        }
                    }
                }
            });
        }
        barrier.wait();
        let mut next = [0u32; 4];
        let mut buf = vec![0; 32768];
        for _ in 0..4000 {
            let info = receiver.unsafe_recv(&mut buf, 5000).unwrap().expect("missing frame");
            let message = protocol::unpack(&buf[..info.len]).unwrap();
            assert_eq!(message.body.len(), 16384);
            let sender = u32::from_le_bytes(message.body[..4].try_into().unwrap()) as usize;
            assert!(sender < 4);
            let seq = u32::from_le_bytes(message.body[4..8].try_into().unwrap());
            assert_eq!(seq, next[sender], "duplicate or out-of-order frame");
            assert!(message.body[8..].iter().all(|b| *b == sender as u8), "torn body");
            next[sender] += 1;
        }
        assert_eq!(next, [1000; 4]);
        assert!(receiver.unsafe_recv(&mut buf, 10).unwrap().is_none());
    });
    drop(receiver);
    switch.stop().unwrap();
    let _ = std::fs::remove_file(format!("/dev/shm{name}"));
}
