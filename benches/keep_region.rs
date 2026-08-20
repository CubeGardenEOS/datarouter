//! KEEP region (layout 13) bench — measures the direct-storage CFO hit path
//! and the write path with arena eviction, plus a zero-copy get_ref/verify.
//!
//! The crossing `protocol_hot_path` / `ipc_*` benches are the regression gate;
//! this standalone binary isolates KEEP latency/throughput numbers for the
//! `docs/benchmark-*.md` record.

use std::hint::black_box;
use std::time::{Duration, Instant};

use blinkcms_datarouter::frontend::{Frontend, FrontendOptions};
use blinkcms_datarouter::switch_frontend::{LogLevel, SwitchConfig, SwitchHandle};

fn best_ns(iterations: usize, mut f: impl FnMut()) -> Duration {
    let mut best = Duration::MAX;
    for _ in 0..7 {
        let started = Instant::now();
        for _ in 0..iterations {
            f();
        }
        best = best.min(started.elapsed());
    }
    best
}

fn main() {
    let name = format!(
        "/dr-keep-bench-{}-{}",
        std::process::id(),
        Instant::now().elapsed().as_nanos()
    );
    let _ = std::fs::remove_file(format!("/dev/shm{}", name));
    let mut sw = SwitchHandle::start(SwitchConfig {
        shm_name: name.clone(),
        ttl_ms: 60000,
        max_ports: 8,
        allow: vec!["core".to_owned()],
        keep_size: Some("8m".to_owned()),
        keep_write_allow: vec!["core".to_owned()],
        log_level: LogLevel::Error,
        ready_timeout: Duration::from_secs(10),
        ..SwitchConfig::default()
    })
    .expect("start switch");

    let core = Frontend::connect_unsafe(FrontendOptions {
        shm_name: name.clone(),
        service_name: "core".to_owned(),
        connect_timeout_ms: 1000,
        recv_timeout_ms: 1000,
        reader_wait_ms: 10,
        ..FrontendOptions::default()
    })
    .expect("connect");

    let key = b"bench:key:query:row";
    let value = vec![0x5Au8; 256];

    // --- Warm up + correctness self-check ---
    core.keep_put(key, &value, 0).expect("put ok");
    let mut out = vec![0u8; 1024];
    let len = core.keep_get(key, &mut out).expect("get ok");
    assert_eq!(&out[..len], &value[..], "keep round-trip correctness");

    let iterations = 500_000;

    // --- keep_get hit (copying, 1 memcpy) ---
    let get_time = best_ns(iterations, || {
        let mut out = vec![0u8; 1024];
        let n = core.keep_get(key, &mut out).expect("get");
        black_box(n);
    });
    let get_ns = get_time.as_nanos() as f64 / iterations as f64;

    // --- keep_get_ref + verify_ref (zero-copy two-phase) ---
    let ref_time = best_ns(iterations, || {
        let (_, n, seq) = unsafe { core.keep_get_ref(key).expect("get_ref") };
        core.keep_verify_ref(seq).expect("verify");
        black_box(n);
    });
    let ref_ns = ref_time.as_nanos() as f64 / iterations as f64;

    // --- keep_put (with ring eviction pressure) ---
    let put_iterations = iterations / 10;
    let mut counter = 0u64;
    let put_time = best_ns(put_iterations, || {
        counter += 1;
        core.keep_put(&counter.to_le_bytes(), &value, 0)
            .expect("put");
    });
    let put_ns = put_time.as_nanos() as f64 / put_iterations as f64;

    let stats = core.keep_stats().expect("stats");
    println!("keep_get hit        : {get_ns:.2} ns/op, {:.3} Mop/s", 1e9 / get_ns);
    println!("keep_get_ref+verify : {ref_ns:.2} ns/op, {:.3} Mop/s", 1e9 / ref_ns);
    println!("keep_put (evictly)  : {put_ns:.2} ns/op, {:.3} Mop/s", 1e9 / put_ns);
    println!(
        "keep_put throughput  : {:.1} MiB/s (256B value)",
        (256.0 * 1e9) / put_ns / (1024.0 * 1024.0)
    );
    println!(
        "keep_stats           : entries={} capacity={} evictions={} hits={} misses={}",
        stats.entry_count, stats.capacity, stats.evictions, stats.hits, stats.misses
    );
    println!("KEEP bench self-check passed");

    core.shutdown();
    let _ = sw.stop();
    let _ = std::fs::remove_file(format!("/dev/shm{}", name));
}
