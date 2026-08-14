# DataRouter 改造后基准（2026-08）

对比对象：同一台机器、同一 benchmark 二进制（`benches/ipc_frontend.rs`）、
默认 512KiB ring。旧实现 = `feb0032`（reader 每帧 unpack→repack，
`MAX_APP_PAYLOAD=60KiB`）；新实现 = 本提交（原始字节入队、512KiB 单帧、
全链路 4 次拷贝、reader 缓冲池）。

基准路径：完整 Rust Frontend（portal Frontend → switch 进程 → core Frontend），
`recv_packed` 取原始 wire 字节。

## 单程吞吐（portal → core）

| payload | 旧实现 | 新实现 | 提升 |
|---|---|---|---|
| 1KiB  | 2.17µs/帧，460k msg/s，449 MiB/s  | 1.36µs/帧，733k msg/s，716 MiB/s  | 1.6× |
| 32KiB | 11.7µs/帧，85k msg/s，2.6 GiB/s   | 3.57µs/帧，280k msg/s，8.6 GiB/s  | 3.3× |
| 64KiB | 166.8µs/帧（2 片 + ACK），6.0k msg/s，375 MiB/s | 9.02µs/帧（单帧），111k msg/s，6.9 GiB/s | 18× |
| 300KiB | 552.9µs/帧（5 片串行 ACK），1.8k msg/s，530 MiB/s | 135.7µs/帧（单帧），7.4k msg/s，2.1 GiB/s | 4.1× |

## pingpong 往返（portal → core → portal）

| payload | 旧实现 p50 | 新实现 p50 |
|---|---|---|
| 64B   | 55.4µs | 54.4µs（持平） |
| 1KiB  | 65.7µs | 72.2µs（持平） |
| 300KiB | 1152µs（每片等 ACK） | 345µs（单帧）→ 3.3× |

## 拷贝预算

`recv_packed` 全链路从 7 次 payload 拷贝降到 4 次：

1. 发送端 packed → TX ring；
2. switch TX ring → 目标 RX ring；
3. RX ring → 接收端 reader 缓冲（唯一接收侧拷贝）；
4. 队列原始字节 → 接收应用。

reader 不再 unpack/repack；接收缓冲由 `FrontendInner::buffer_pool` 回收，
稳态接收零分配。`Frontend::recv`（Message API）同样只有 4 次拷贝。

## 复现

```bash
DATAROUTER_SWITCH_BIN=$(find target/release/build -name datarouter-switch | head -1) \
  cargo bench --bench ipc_frontend
```

机器噪声说明：本机曾残留多个空转 switch 进程（各 ~17% CPU），清理后
各 payload 的帧耗时仍有 ±30% 抖动；上表取清理后的单次干净运行。

## unsafe 直接数据面对比（2026-08 复测）

同机、3 次中位数（后台有 Gradle daemon 噪声，正常路径抖动 ±30–60%），
unsafe 路径把拷贝从 4 次降到 2 次、绕过 switch 路由，详见
`docs/benchmark-unsafe-2026-08.md`：

| 场景 | 无 unsafe | 有 unsafe | 提升 |
|---|---|---|---|
| pingpong 64B p50   | 30.0µs   | 3.06µs   | 9.8× |
| pingpong 1KiB p50  | 63.2µs   | 3.18µs   | 19.9× |
| pingpong 300KiB p50 | 318.7µs | 29.5µs   | 10.8× |
| 单程 32KiB 吞吐     | 6.6 GiB/s | 10.1 GiB/s | 1.5× |
| 单程 64KiB 吞吐     | 5.1 GiB/s | 14.0 GiB/s | 2.7× |
| 单程 300KiB 吞吐    | 1.8 GiB/s | 18.2 GiB/s | 9.9× |
| 单程 1KiB 吞吐      | 513 MiB/s | 569 MiB/s | 持平（±10%） |
