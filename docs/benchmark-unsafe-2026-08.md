# DataRouter unsafe 直接数据面基准（2026-08）

`unsafe` 是缺省 Cargo feature（直接数据面，2026-08 翻转；旧拷贝模式用
`--no-default-features` 保留）。它把数据面从
switch 路由改为发送端直接指针写目标 RX ring（形似 port.c 对共享内存的指针操作），
switch 仅保留控制面（注册/注销/TTL/死进程回收）与控制请求中断。全链路 payload
拷贝从 4 次降到 2 次：

1. 发送端 packed → 目标 RX ring（`datarouter_unsafe_send` 直接 append + 直接 post
   `rx_signal[target]`）；
2. RX ring → 接收端应用 buffer（直接 `datarouter_recv`，无 reader 线程、无队列）。

## 测试结果（本机，两种配置）

| 配置 | 单元测试 | unsafe 集成测试 |
|---|---|---|
| `cargo test`（缺省 = unsafe） | 68 通过 | 6 通过（往返/混合/撕裂/超尺寸/…） |
| `cargo test --no-default-features`（safe） | 68 通过 | 0（feature 关闭，cfg 排除） |

## 跑分结果（中位数，各 3 次）

机器：AMD Ryzen 5 3600（12 线程），默认 512KiB ring，同一 switch 二进制。
测量时机器有后台负载（Gradle daemon ~57% CPU + 两个 pi 进程），正常路径抖动
±30–60%；unsafe 路径三次跑分稳定（抖动 <10%）。下表取 3 次中位数。

### 单程吞吐（portal → core）

| payload | 无 unsafe 正常路径（4 拷贝） | 有 unsafe（2 拷贝） | 提升 |
|---|---|---|---|
| 1KiB   | 1.90µs/帧，525k msg/s，513 MiB/s    | 1.72µs/帧，583k msg/s，569 MiB/s   | ≈持平（±10% 噪声内） |
| 32KiB  | 4.60µs/帧，218k msg/s，6.6 GiB/s   | 3.04µs/帧，329k msg/s，10.1 GiB/s  | 1.5× |
| 64KiB  | 11.9µs/帧，84k msg/s，5.1 GiB/s    | 4.36µs/帧，229k msg/s，14.0 GiB/s  | 2.7× |
| 300KiB | 155.6µs/帧，6.4k msg/s，1.8 GiB/s  | 15.7µs/帧，64k msg/s，18.2 GiB/s   | 9.9× |

### pingpong 往返（portal → core → portal）

| payload | 无 unsafe 正常路径 p50 | 有 unsafe p50 | 提升 |
|---|---|---|---|
| 64B    | 30.0µs | 3.06µs | 9.8× |
| 1KiB   | 63.2µs | 3.18µs | 19.9× |
| 300KiB | 318.7µs | 29.5µs | 10.8× |

unsafe 路径 64B/1KiB 往返 p50 ≈ 3.1µs，接近共享内存读写 + 两次 memcpy 的物理
下限；300KiB 单程吞吐 18.2 GiB/s，受限于是 memcpy 带宽（2 次 300KiB 拷贝）。

## 1KiB 单程为什么持平

`benches/ipc_unsafe.rs` 与正常 bench 一样是单线程交替 send/recv。正常路径靠 core
的 reader 线程后台把帧预取进队列，`recv_packed` 直接弹出，pipeline 更深；unsafe
路径没有 reader/队列，`unsafe_recv` 每帧直接读 ring，发送与接收在同一线程内串行。
小帧时系统调用与原子操作开销占比高，二者相当（本次中位数 unsafe 略快，在噪声
范围内）。大帧（≥32KiB）memcpy 占主导，unsafe 少 2 次拷贝的优势完全体现。应用
若需要小帧高吞吐，可自行起接收线程把 `unsafe_recv` 挪到后台（数据面仍是 2 次拷贝）。

## 约束（开发中特性）

- 同一目标 RX ring 只允许一个逻辑写者（与 switch 路由的单写者语义一致）；
  多进程并发直写同一目标未做串行化。
- 目标 ring 满时返回 `DATAROUTER_ERR_FULL`，不走 switch 的 blocked 重排协议。
- 单帧负载 ≤ `DATAROUTER_MAX_APP_PAYLOAD`，无 chunking/ACK。
- 目标槽位在写入期间被 io_users 钉住（阻止释放/复用竞态）；写后重校验注册身份，
  若目标已注销（DRAINING）帧可能被最终释放时丢弃。

## 复现

```bash
# 无 unsafe 正常路径基线（safe 模式）
cargo bench --bench ipc_frontend --no-default-features
# 有 unsafe 直接数据面（缺省）
cargo bench --bench ipc_unsafe
# 测试（缺省 = unsafe；safe 用 --no-default-features）
cargo test
cargo test --no-default-features
```
