//! Validation throughput bench for the C single-source-of-truth validation
//! path (`protocol::validate_tlvs` and `protocol::raw_frame_layout`, both of
//! which delegate to `src/validation.c` via FFI).
//!
//! Measures:
//!   - `validate_tlvs` on a realistic multi-field TLV block (business
//!     request var_data).
//!   - `raw_frame_layout` (exact frame-header/length validation) on a frame
//!     with a non-trivial body.
//!
//! This is a self-checking standalone binary (harness = false); it asserts the
//! C-backed validation stays correct and reports ns/op and ops/stats for the
//! bench record. The org benchmark suite (`datarouter-benchmark`) is the
//! cross-process regression gate; this bench isolates the pure validation CPU
//! cost.

use std::hint::black_box;
use std::time::{Duration, Instant};

use blinkcms_datarouter::protocol;

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
    // A representative business-request var_data block: module/action/request_id
    // (+ trace_id for a realistic multi-field block), built once.
    let mut vd = protocol::VarDataBuilder::new();
    vd.module("public")
        .action("assets.read")
        .request_id("request-0001-ffff-1234567890ab")
        .trace_id("trace-abcdef0123456789");
    let var_data = vd.build();
    let vd_len = var_data.len();

    // A full frame to validate with raw_frame_layout (exact length match).
    let body = vec![7_u8; 64 * 1024];
    let mut frame = Vec::with_capacity(protocol::HEADER_SIZE + var_data.len() + body.len());
    frame.push(protocol::MSG_BUSINESS_RESPONSE);
    frame.push(0);
    frame.extend_from_slice(&(var_data.len() as u16).to_le_bytes());
    frame.extend_from_slice(&(body.len() as u32).to_le_bytes());
    frame.extend_from_slice(&var_data);
    frame.extend_from_slice(&body);

    let iterations = 500_000;

    let tlv_time = best_ns(iterations, || {
        // validate_tlvs delegates to the C validator.
        let _ = black_box(protocol::validate_tlvs(&var_data).is_ok());
    });
    let tlv_ns = tlv_time.as_nanos() as f64 / iterations as f64;

    let frame_time = best_ns(iterations, || {
        // raw_frame_layout delegates to the C frame validator.
        let _ = black_box(protocol::raw_frame_layout(&frame).is_some());
    });
    let frame_ns = frame_time.as_nanos() as f64 / iterations as f64;

    // Receive-path classification: frame header validation + single-frame
    // detection (which now happen in one C pass; the old path did a separate
    // FFI validate_tlvs call plus a redundant Rust TLV re-scan).
    let recv_time = best_ns(iterations / 2, || {
        let _ = black_box(protocol::raw_frame_layout(&frame));
        let _ = black_box(protocol::raw_is_single_frame(
            &frame[protocol::HEADER_SIZE..protocol::HEADER_SIZE + var_data.len()],
        ));
    });
    let recv_ns = recv_time.as_nanos() as f64 / (iterations / 2) as f64;

    println!("validate_tlvs(blocksize={vd_len}B): {tlv_ns:.2} ns/op, {:.0} Mop/s", 1e9 / tlv_ns);
    println!(
        "raw_frame_layout(framesize={}B): {frame_ns:.2} ns/op, {:.0} Mop/s",
        frame.len(),
        1e9 / frame_ns
    );
    println!(
        "recv classify (frame + single-frame): {recv_ns:.2} ns/op, {:.0} Mop/s",
        1e9 / recv_ns
    );

    // Correctness self-check (the returned value is consumed so the optimizer
    // cannot elide the FFI calls).
    assert!(protocol::validate_tlvs(&var_data).is_ok());
    assert!(protocol::raw_frame_layout(&frame).is_some());
    assert!(protocol::validate_tlvs(&var_data[..var_data.len() - 1]).is_err());
    assert!(protocol::raw_is_single_frame(&var_data));
    println!("validation self-check passed");
}
