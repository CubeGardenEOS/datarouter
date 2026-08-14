# DataRouter unsafe 直接数据面基准（2026-08）

`unsafe` 是开发中 Cargo feature（`--features unsafe`，默认关闭）。它把数据面从
switch 路由改为发送端直接指针写目标 RX ring（形似 port.c 对共享内存的指针操作），
switch 仅保留控制面（注册/注销/TTL/死进程回收）与控制请求中断。全链路 payload
拷贝从 4 次降到 2 次：

1. 发送端 packed → 目标 RX ring（`datarouter_unsafe_send` 直接 append + 直接 post
   `rx_signal[target]`）；
2. RX ring → 接收端应用 buffer（直接 `datarouter_recv`，无 reader 线程、无队列）。

对比对象：同一台机器、同一 benchmark 结构（`benches/ipc_frontend.rs` 正常路径 vs
`benches/ipc_unsafe.rs` unsafe 路径）、默认 512KiB ring、同一 switch 二进制。

## 单程吞吐（portal → core）

| payload | 正常路径（4 次拷贝） | unsafe（2 次拷贝） | 提升 |
|---|---|---|---|
| 1KiB   | 1.454µs/帧，687k msg/s，671 MiB/s   | 1.758µs/帧，569k msg/s，555 MiB/s   | 0.83×（持平） |
| 32KiB  | 5.155µs/帧，194k msg/s，6.1 GiB/s   | 3.197µs/帧，313k msg/s，9.8 GiB/s   | 1.6× |
| 64KiB  | 12.97µs/帧，77k msg/s，4.8 GiB/s    | 4.513µs/帧，222k msg/s，13.8 GiB/s  | 2.9× |
| 300KiB | 145.8µs/帧，6.9k msg/s，2.0 GiB/s   | 15.85µs/帧，63k msg/s，18.5 GiB/s   | 9.2× |

## pingpong 往返（portal → core → portal）

| payload | 正常路径 p50 | unsafe p50 | 提升 |
|---|---|---|---|
| 64B    | 28.2µs  | 3.10µs  | 9.1× |
| 1KiB   | 71.3µs  | 3.25µs  | 22×  |
| 300KiB | 347µs   | 29.6µs  | 11.7× |

64B/1KiB 往返 p50 ≈ 3.1–3.2µs，接近共享内存读写 + 两次 memcpy 的物理下限；
300KiB 单程吞吐达 18.5 GiB/s，受限于是 memcpy 带宽（2 次 300KiB 拷贝）。

## 1KiB 单程为什么持平

`benches/ipc_unsafe.rs` 与正常 bench 一样是单线程交替 send/recv。正常路径靠 core
的 reader 线程后台把帧预取进队列，`recv_packed` 直接弹出，pipeline 更深；unsafe
路径没有 reader/队列，`unsafe_recv` 每帧直接读 ring，发送与接收在同一线程内串行。
小帧时系统调用与原子操作开销占比高，二者相当。大帧（≥32KiB）memcpy 占主导，
unsafe 少 2 次拷贝的优势完全体现。应用若需要小帧高吞吐，可自行起接收线程把
`unsafe_recv` 挪到后台（数据面仍是 2 次拷贝）。

## 约束（开发中特性）

- 同一目标 RX ring 只允许一个逻辑写者（与 switch 路由的单写者语义一致）；
  多进程并发直写同一目标未做串行化。
- 目标 ring 满时返回 `DATAROUTER_ERR_FULL`，不走 switch 的 blocked 重排协议。
- 单帧负载 ≤ `DATAROUTER_MAX_APP_PAYLOAD`，无 chunking/ACK。
- 目标槽位在写入期间被 io_users 钉住（阻止释放/复用竞态）；写后重校验注册身份，
  若目标已注销（DRAINING）帧可能被最终释放时丢弃。

## 复现

```bash
# 正常路径基线
DATAROUTER_SWITCH_BIN=$(find target/release/build -name datarouter-switch | head -1) \
  cargo bench --bench ipc_frontend --features unsafe
# unsafe 直接数据面
DATAROUTER_SWITCH_BIN=$(find target/release/build -name datarouter-switch | head -1) \
  cargo bench --bench ipc_unsafe --features unsafe
```
