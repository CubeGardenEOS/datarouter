//! ABI 兼容性钉扎测试。
//!
//! 锁定 shared-memory layout（layout 12）与 frontend ABI 的常量，防止 C 库 /
//! Rust crate / JNI（`jni/src/lib.rs` 的 `compatibility_fingerprint_is_fixed_*`）/
//! Python wheel（`python/tests/test_native_abi.py`）之间的绑定漂移。任何 layout
//! 或 ABI 变更必须同时更新这里与上述绑定侧的断言。

use blinkcms_datarouter::frontend::ffi::DATAROUTER_FRONTEND_ABI_VERSION;

#[test]
fn layout_12_constants_are_pinned() {
    // channel 总大小：默认 2^19。
    assert_eq!(blinkcms_datarouter::DATAROUTER_CHANNEL_SIZE, 524_288);
    // ring 数据容量 = channel 大小（2 的幂，环形回绕用掩码而非除法）。
    assert_eq!(blinkcms_datarouter::DATAROUTER_RING_CAPACITY, 524_288);
    assert!(blinkcms_datarouter::DATAROUTER_RING_CAPACITY.is_power_of_two());
    assert_eq!(
        blinkcms_datarouter::DATAROUTER_RING_CAPACITY
            & (blinkcms_datarouter::DATAROUTER_RING_CAPACITY - 1),
        0
    );
    // 8 字节原子 ring 头 + 8 字节帧头。
    assert_eq!(blinkcms_datarouter::DATAROUTER_RING_HEADER_SIZE, 8);
    assert_eq!(blinkcms_datarouter::DATAROUTER_FRAME_HEADER_SIZE, 8);
    // 最大单帧负载 = 2^19 - 8。
    assert_eq!(
        blinkcms_datarouter::DATAROUTER_MAX_APP_PAYLOAD,
        524_288 - 8
    );
    assert_eq!(blinkcms_datarouter::DATAROUTER_SERVICE_NAME_MAX, 31);
}

#[test]
fn frontend_abi_is_v2() {
    assert_eq!(DATAROUTER_FRONTEND_ABI_VERSION, 2);
}

#[test]
fn c_library_links_and_reports_version() {
    // 通过 FFI 调用 C 库本身（version 符号来自链接的 staticlib），
    // 证明 C 与 Rust 在同一二进制内一致链接。
    let version = blinkcms_datarouter::version();
    let bytes = version.to_bytes();
    assert!(
        bytes.starts_with(b"0.2."),
        "unexpected C ABI version string: {:?}",
        bytes
    );
}

#[test]
fn copy_strategy_defaults_to_scalar() {
    // 未显式 DATAROUTER_COPY=avx2 时，缺省必须是 scalar（libc memcpy）。
    if std::env::var("DATAROUTER_COPY").as_deref() != Ok("avx2") {
        assert_eq!(blinkcms_datarouter::copy_strategy(), "scalar");
    }
}
