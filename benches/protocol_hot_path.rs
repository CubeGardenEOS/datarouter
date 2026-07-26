use std::hint::black_box;
use std::time::{Duration, Instant};

use blinkcms_datarouter::protocol::{build_business_response, pack, pack_business_response_into};

fn measure(size: usize, iterations: usize) -> (Duration, Duration) {
    let payload = vec![7_u8; size];
    let mut best_legacy = Duration::MAX;
    let mut best_direct = Duration::MAX;
    for _ in 0..5 {
        let started = Instant::now();
        for _ in 0..iterations {
            let message = build_business_response(
                "public",
                "asset.read",
                "r1",
                "c1",
                black_box(&payload),
                false,
            );
            black_box(pack(&message).unwrap());
        }
        best_legacy = best_legacy.min(started.elapsed());

        let mut buffer = Vec::with_capacity(size + 128);
        let started = Instant::now();
        for _ in 0..iterations {
            pack_business_response_into(
                &mut buffer,
                "public",
                "asset.read",
                "r1",
                "c1",
                black_box(&payload),
                false,
            )
            .unwrap();
            black_box(&buffer);
        }
        best_direct = best_direct.min(started.elapsed());
    }
    (best_legacy, best_direct)
}

fn main() {
    for (size, iterations) in [(64 * 1024, 2_000), (512 * 1024, 300)] {
        let (legacy, direct) = measure(size, iterations);
        let legacy_copied = size * 2;
        let direct_copied = size;
        println!(
            "size={size} legacy={legacy:?} direct={direct:?} legacy_copied={legacy_copied} direct_copied={direct_copied}"
        );
        assert!(direct_copied < legacy_copied);
        assert!(
            direct.as_nanos() * 100 <= legacy.as_nanos() * 105,
            "direct response pack regressed by more than 5%"
        );
    }
}
