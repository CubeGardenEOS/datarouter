# DataRouter

DataRouter 是独立的本地共享内存传输层，供 BizPilot、Shiroitori 和其他同机服务以 Git submodule 方式引用。它不是网络协议，不解析业务 JSON，也不负责业务鉴权；payload 对 DataRouter 来说是不透明字节。

当前包含三部分：

- `datarouter-switch`：唯一创建和初始化 shared memory 的交换进程。
- C Port API：应用侧 ABI，负责打开 shared memory、注册服务名、发送和接收帧。
- Rust crate `blinkcms-datarouter`：通过 Cargo 编译 C Port 静态库并暴露原始 FFI 绑定。

使用者必须先独立运行 `datarouter-switch`，再由 Core、Edge Node 或其他 worker 调用 Port API 连接。Rust crate 不启动、不嵌入 `datarouter-switch`。

## 消费者集成边界

- 每个消费者通过自己的部署工具启动独立 `datarouter-switch`，并固定 allowlist、shared-memory 名称和权限。
- 应用进程只使用 Rust `Frontend`、Java JNI SDK 或 C Port API；不得嵌入 switch。
- Shiroitori 固定使用 `SHD2`/`MSG_CONTROL` 数据面，详见其 ADR-0011。

## 构建

DataRouter 是 C11 共享内存传输，提供 Linux 和 Windows 平台后端。BizPilot 仍提供其项目级的 `python build/main.py datarouter …` 包装；Shiroitori 通过 `mojang/setup.py` 构建部署所需的 Linux switch/JNI 资源。

```bash
python build/main.py datarouter build
python build/main.py datarouter test
```

Linux 默认使用 `PLATFORM=linux`。Windows 目标会选择 Win32 file mapping 后端：

```bat
python build/main.py --platform windows/x86_64 datarouter build
python build/main.py --platform windows/x86_64 datarouter windows-test
```

动态生成的 Make 程序主要产物：

- `build/libdatarouter.so`
- `build/libdatarouter.a`
- `build/datarouter-switch`

Linux 后端通过 `shm_open`、`ftruncate`、`fchmod` 和 `mmap` 使用 POSIX shared memory。Windows 后端使用 Win32 file mapping API。

## IPC Benchmark

### 默认基准

默认 `build/main.py datarouter bench` 使用主构建产物；所有构建的 channel size 均为 `512KiB`：

```bash
python build/main.py datarouter bench
```

默认基准产物：

- `build/bench_pingpong`
- `build/bench_burst`

### 512KiB Ring + CPU-2 并行调度实验

`ipcbench-512k` 是只用于 IPC 压测的实验目标，输出目录为 `build-ipcbench-512k/`。它和主构建使用相同的 `512KiB` ring，只为实验 switch 保留更激进的自旋、调度和 CPU 绑定参数，不再覆盖 ring 容量。

```bash
python build/main.py datarouter ipcbench-512k
```

实验产物：

- `build-ipcbench-512k/datarouter-switch`
- `build-ipcbench-512k/libdatarouter.a`
- `build-ipcbench-512k/bench_ipc_multichannel_512k`

#### 工具模型

`bench_ipc_multichannel_512k` 默认按运行时在线 CPU 数自动分配并行度：

- `usable_cpus = max(1, online_cpus - 2)`，默认使用 CPU `0..usable_cpus-1`，留下最高编号的 2 个 CPU 不参与本工具。
- `spin_cpus = ceil(usable_cpus / 4)`；当可用 CPU 太少时至少保留 1 个 app CPU。
- `app_cpus = usable_cpus - spin_cpus`。
- `lane_count = usable_cpus`。
- `--inflight` 是全局业务并发窗口，按 lane 均分为 `per_lane_inflight = ceil(inflight / lane_count)`；实际窗口为 `effective_inflight = per_lane_inflight * lane_count`。

每个 lane 会启动 3 个独立的 datarouter switch/shared memory：

| 类型 | Shared memory | 说明 |
| --- | --- | --- |
| GET miss | `/datarouter-ipcbench-get-{lane}` | 仅缓存未命中的 GET 进入 IPC |
| POST | `/datarouter-ipcbench-post-{lane}` | mutation 请求，不走缓存 |
| ws | `/datarouter-ipcbench-ws-{lane}` | 模拟 WebSocket 事件/请求吞吐 |

每个 lane 内各注册一组 fake `cms` 和 fake `core` port。Coordinator 启动 `3 * lane_count` 个 switch，switch 进程 round-robin 绑定到 `spin_cpus`；fake CMS/Core 进程内各启动 `lane_count` 个 lane 线程，线程 round-robin 绑定到 `app_cpus`。每个 lane 线程只打开自己的 shm port，不共享 raw port。

CMS 按请求类型选择 channel；GET cache hit 在 CMS 本地完成，不进入 IPC。Core 不模拟业务处理延迟；`--latency-us` 只模拟请求者网络延迟，由 CMS 在完成时间上补齐。

这个工具不模拟真实长期 WebSocket 文件描述符、心跳、订阅 fanout 或浏览器连接内存，只用于请求/事件吞吐与 IPC 背压实验。

#### USAGE

```bash
lib/datarouter/build-ipcbench-512k/bench_ipc_multichannel_512k \
  --duration 15 \
  --inflight 29696 \
  --payload-bytes 1024 \
  --latency-us 50000 \
  --cpus auto \
  --rss-limit-bytes 1073741824 \
  --switch-bin lib/datarouter/build-ipcbench-512k/datarouter-switch
```

常用参数：

| 参数 | 默认值 | 说明 |
| --- | --- | --- |
| `--duration` | `15` | CMS 主动发送窗口，单位秒。 |
| `--inflight` | `1024` | 最大并发 in-flight 请求窗口。 |
| `--payload-bytes` | `1024` | 每个 IPC 请求/响应的 payload 字节数。 |
| `--latency-us` | `50000` | 请求者网络延迟，单位微秒。 |
| `--cpus` | `auto` | CPU 分配。`auto` 使用在线 CPU 数减 2；也可传 `0-3,6` 这类显式 CPU 列表。 |
| `--spin-cpus` | 空 | 显式指定 switch/scheduler 自旋 CPU 集合，必须与 `--app-cpus` 一起使用。 |
| `--app-cpus` | 空 | 显式指定 fake CMS/Core lane 线程 CPU 集合，必须与 `--spin-cpus` 一起使用。 |
| `--rss-limit-bytes` | `1073741824` | switch + fake core + fake cms 合计 RSS 限制。 |
| `--switch-bin` | `build-ipcbench-512k/datarouter-switch` | 实验 switch 路径。 |
| `--get-percent` | `80` | GET 请求占比。 |
| `--post-percent` | `5` | POST 请求占比。 |
| `--ws-percent` | `15` | ws 事件/请求占比。 |
| `--get-hit-percent` | `95` | GET cache hit 占比。 |

输出为单行 JSON，关键字段：

| 字段 | 说明 |
| --- | --- |
| `sent` / `completed` | CMS 发送和完成的业务请求数。稳定档要求二者相等。 |
| `send_errors` / `recv_errors` | 业务侧发送/接收错误数。稳定档要求均为 0。 |
| `throughput_s` | 完成请求数 / 总耗时。 |
| `effective_inflight` / `per_lane_inflight` | lane 均分后的实际全局窗口和每 lane 窗口。 |
| `online_cpus` / `usable_cpus` | 运行时在线 CPU 数，以及本工具实际使用的 CPU 数。 |
| `spin_cpus` / `app_cpus` | switch/scheduler 自旋 CPU 数和 fake CMS/Core app CPU 数。 |
| `lane_count` / `scheduler_cpu_count` | lane 数和调度 CPU 数。 |
| `max_rss_bytes` | 所有 switch + fake core + fake cms 进程的最大合计 RSS。 |
| `latency` | 总完成延迟统计，包含请求者网络延迟。 |
| `ipc_latency` | 只统计进入 IPC 的请求延迟；GET hit 不包含在内。 |
| `by_kind` | `GET`、`GET_hit`、`GET_miss`、`POST`、`ws` 分类型延迟。 |
| `by_channel` | GET miss、POST、ws 三个 channel 的吞吐、send error 和 IPC 延迟。 |
| `by_lane` | 每个 lane 的 sent/completed/errors/throughput 简要统计。 |

稳定判断：

- `send_errors=0`
- `recv_errors=0`
- `completed=sent`
- drain 不超过主测试窗口后 2 秒
- `max_rss_bytes < rss-limit-bytes`
- 总 p99 接近 `latency-us + IPC p99`，无长尾暴涨

业务可接受判断：

- 严格稳定档仍要求 `send_errors=0`、`recv_errors=0`、`completed=sent`。
- 本轮容量评估同时记录业务可接受档：偶发错误总量 `send_errors + recv_errors + (sent - completed)` 不超过 `sent` 的 `5%`。
- 业务可接受档不是无损稳定上限，文档中会明确标注。

#### 2026-06-17 256KB Ring 历史结果

固定参数：

- `duration=15s`
- `payload_bytes=1024`
- `GET/POST/ws = 80%/5%/15%`
- `GET cache hit = 95%`
- `latency=50ms`
- `cpu=0-3`
- `rss_limit=400MiB`
- channel size：`256KB`
- channel 拆分：GET miss、POST、ws 各自独立 switch/shm；GET hit 本地命中

结果汇总：

| inflight | 结果 | throughput | send_errors | recv_errors | IPC p99 | max RSS |
| ---: | --- | ---: | ---: | ---: | ---: | ---: |
| 1024 | 稳定 | 20.4k/s | 0 | 0 | 4.9ms | 25.7MB |
| 2048 | 稳定 | 40.8k/s | 0 | 0 | 6.1ms | 33.9MB |
| 4096 | 稳定 | 81.6k/s | 0 | 0 | 1.8ms | 69.0MB |
| 8192 | 稳定 | 147.7k/s | 0 | 0 | 5.0ms | 104.7MB |
| 16384 | 稳定 | 163.2k/s | 0 | 0 | 4.7ms | 130.8MB |
| 24576 | 稳定 | 160.2k/s | 0 | 0 | 4.9ms | 114.8MB |
| 28672 | 稳定 | 156.4k/s | 0 | 0 | 4.8ms | 132.1MB |
| 29696 | 稳定 | 161.8k/s | 0 | 0 | 4.6ms | 106.6MB |
| 30208 | 失败 | 72.2k/s | 58,740 | 464,424 | 4.7ms | 73.1MB |
| 30720 | 失败 | 134.1k/s | 18,605 | 391,047 | 4.7ms | 86.0MB |
| 32768 | 失败 | 41.5k/s | 60,148 | 461,378 | 4.3ms | 40.2MB |

历史结论：

- 256KB ring 的严格稳定业务并发上限约为 `29696` in-flight。
- `30208` 起业务侧出现大量 `send_errors` 和 `recv_errors`。
- 吞吐平台期约为 `160k req/s`；RSS 远低于 `400MiB`，瓶颈不是内存容量。

#### 2026-06-17 512KB Ring + 显式 CPU 绑定结果

固定参数：

- `duration=15s`
- `payload_bytes=1024`
- `GET/POST/ws = 80%/5%/15%`
- `GET cache hit = 95%`
- `latency=50ms`
- channel size：`512KB`
- 机器在线 CPU：`12`
- CPU 分配：`--spin-cpus 0-7 --app-cpus 8-11`
- lane：`lane_count=12`，每个 lane 独立 GET miss、POST、ws 三组 shm/switch

1GiB RSS 限制结果：

| inflight | 结果 | throughput | error rate | send_errors | recv_errors | IPC p99 | max RSS |
| ---: | --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 29696 | 严格稳定 | 567.7k/s | 0.00000% | 0 | 0 | 19.6ms | 858.9MB |
| 31744 | 严格稳定 | 604.0k/s | 0.00000% | 0 | 0 | 20.7ms | 896.6MB |
| 32768 | 严格稳定 | 622.7k/s | 0.00000% | 0 | 0 | 21.3ms | 935.5MB |
| 49152 | RSS 限制触发 | - | - | - | - | - | >1GiB |

4GiB RSS 限制结果：

| inflight | 结果 | throughput | error rate | send_errors | recv_errors | IPC p99 | max RSS |
| ---: | --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 49152 | 严格稳定 | 708.4k/s | 0.00000% | 0 | 0 | 35.2ms | 1.011GiB |
| 65536 | 业务可接受 | 703.7k/s | 0.00017% | 19 | 0 | 35.2ms | 1.014GiB |
| 98304 | 严格稳定 | 708.9k/s | 0.00000% | 0 | 0 | 35.2ms | 1.016GiB |
| 131072 | 业务可接受 | 707.0k/s | 0.00004% | 4 | 0 | 35.3ms | 1.025GiB |
| 196608 | 严格稳定 | 703.8k/s | 0.00000% | 0 | 0 | 35.2ms | 1.046GiB |
| 262144 | 严格稳定 | 700.2k/s | 0.00000% | 0 | 0 | 35.3ms | 1.039GiB |
| 524288 | 严格稳定 | 709.6k/s | 0.00000% | 0 | 0 | 35.1ms | 1.130GiB |

本轮结论：

- 512KB ring + 显式 CPU 绑定将吞吐平台从 256KB 历史测试的约 `160k req/s` 提升到约 `700k req/s`。
- `--spin-cpus 0-7 --app-cpus 8-11` 后，switch/scheduler 自旋与 fake CMS/Core 业务线程隔离，性能提升主要来自调度隔离和自旋 CPU 供给，而不只是 ring 变大。
- 1GiB RSS 下，`32768` 是本轮严格稳定档；`49152` 触发 RSS 限制，应记录为 1GiB 内存墙。
- 4GiB RSS 下，`524288` in-flight 仍严格稳定，但吞吐没有继续随并发增加，说明系统已经进入 CPU/调度平台期。
- `65536` 和 `131072` 出现极少量 ws send error，错误率远低于 `5%`，按业务口径可接受，但不是无损稳定档。

## Cargo 包

Port 库也可以作为 Cargo 包编译、测试和打包。Cargo 会把 C Port 源码编译为静态 native library，再链接到 Rust crate。

```bash
cd lib/datarouter
cargo build
cargo test
cargo package
```

路径依赖示例：

```toml
[dependencies]
blinkcms-datarouter = { path = "lib/datarouter" }
```

Rust FFI 使用示例：

```rust
let options = blinkcms_datarouter::frontend::FrontendOptions {
    service_name: "portal".to_owned(),
    ..Default::default()
};
```

Cargo 包只暴露应用侧 Port ABI。需要创建 shared memory 并路由帧的进程仍要用 Makefile 构建并运行 `datarouter-switch`。

原生构建要求：

- Linux：C11 编译器和 `ar`。
- Windows：MSVC `cl`/`lib`，或兼容的 `CC`/`AR` 环境。

## 协议层

`lib/datarouter` 包含协议编解码器，实现业务消息抽象。协议逻辑用 Rust 编写（`src/protocol.rs`），通过 C ABI 导出（`src/protocol_ffi.rs`、`include/datarouter_protocol.h`）。

### 核心概念

- **Module**：命名空间（`admin`、`public`、`agent`）
- **Action**：操作名（`auth.login`、`products.list`）
- **Payload**：原始业务数据（通常是 JSON）

### 消息类型

| 类型 | 值 | 描述 |
|------|-----|------|
| `MSG_BUSINESS_REQUEST` | `0x07` | 业务请求 |
| `MSG_BUSINESS_RESPONSE` | `0x8C` | 业务响应 |
| `MSG_CHUNK_ACK` | `0x05` | 分块确认 |
| `MSG_ROUTES_REFRESH` | `0x85` | 路由刷新 |
| `MSG_CACHE_INVALIDATE` | `0x87` | 缓存失效 |
| `MSG_CONTROL` | `0x8B` | 控制消息 |
| `MSG_LOG_NOTICE` | `0x8D` | 日志通报占位 |

### Rust 使用

```rust
use blinkcms_datarouter::protocol;

let msg = protocol::build_business_request("admin", "auth.login", "req-1", "ws-1", None, &payload);
let bytes = protocol::pack(&msg)?;
port.send("core", &bytes)?;
```

### C ABI

协议层导出以下 C 函数：

```c
// 消息生命周期
datarouter_message* datarouter_msg_new(uint8_t type);
void datarouter_msg_free(datarouter_message *msg);

// TLV 构建
void datarouter_tlv_put_string(datarouter_message *msg, uint8_t tag, const char *value);
void datarouter_tlv_put_bytes(datarouter_message *msg, uint8_t tag, const void *data, size_t len);

// 线格式
int datarouter_msg_pack(const datarouter_message *msg, void *out_buf, size_t out_capacity, size_t *out_len);
int datarouter_msg_unpack(const void *data, size_t len, datarouter_message **out);

// 业务消息构建
datarouter_message* datarouter_build_business_request(...);
datarouter_message* datarouter_build_business_response(...);
```

完整 API 参见 `include/datarouter_protocol.h`。

## 应用 ABI（Frontend 层）

业务代码只使用 `include/datarouter_frontend.h` 中的 frontend ABI。frontend 打开并注册 port，启动唯一 raw 读循环，并统一处理 chunk ACK、重组、完整消息队列、keepalive 和 registry snapshot：

```c
datarouter_frontend *frontend;
datarouter_frontend_options opts = {
    .shm_name = DATAROUTER_DEFAULT_SHM_NAME,
    .service_name = "MLHelper",
    .connect_timeout_ms = 3000,
};

datarouter_frontend_open_v2(
    &opts,
    sizeof(opts),
    DATAROUTER_FRONTEND_ABI_VERSION,
    &frontend
);
datarouter_frontend_send(frontend, "Core", wire_bytes, wire_len, 0);
datarouter_frontend_recv(frontend, buffer, sizeof(buffer), &info, 3000);
datarouter_frontend_registry_snapshot(frontend, entries, capacity, &count);
datarouter_frontend_stats stats;
datarouter_frontend_stats_snapshot_v2(
    frontend,
    &stats,
    sizeof(stats),
    DATAROUTER_FRONTEND_ABI_VERSION
);
datarouter_frontend_close(frontend);
```

应用层只看到三类能力：

- 注册：`datarouter_frontend_open` / `datarouter_frontend_close`
- 上行：`datarouter_frontend_send`
- 下行：`datarouter_frontend_recv`
- 管理查询：`datarouter_frontend_registry_snapshot`

chunk、ACK、重组、keepalive 和 raw poll/drain 均留在 frontend 内部。

常用函数：

| 函数 | 行为 |
| --- | --- |
| `datarouter_frontend_open_v2` | 校验 struct size/ABI version，打开 switch shared memory 并注册服务名。 |
| `datarouter_frontend_send` | 发送一个完整 packed protocol message；大消息自动分块并等待 ACK。 |
| `datarouter_frontend_recv` | 阻塞接收一个完整 packed protocol message；缓冲区不足时保留队首消息，并通过 `out_info.len` 返回所需容量。 |
| `datarouter_frontend_registry_snapshot` | 从已注册 frontend 查询当前活跃服务。 |
| `datarouter_frontend_stats_snapshot_v2` | 在 size/version 校验后返回 frontend 的累计发送、接收、ACK、队列和错误计数。 |
| `datarouter_frontend_close` | 关闭 frontend 和底层 port。 |

### Raw Port 边界

`datarouter_open` / `datarouter_logon` / `datarouter_send` / `datarouter_recv` / `datarouter_wait` / `datarouter_drain` 只保留给 `lib/datarouter` 内部测试、benchmark 和 switch 调试边界，不从 Go/Python/Rust 应用入口暴露。

帧头固定 8 字节：

- `uint32_t route_id`
- `uint32_t payload_len`

payload 字节不透明。JSON envelope 数组会原样透传，router 不解析也不改写业务内容。

## 内存布局

`datarouter-switch` 创建一个 shared-memory object。对象由控制面和每个活跃 port slot 的一组 RX/TX channel 组成，每个 channel 连续 `512KiB`：

```text
control plane
slot0 RX 512KiB
slot0 TX 512KiB
slot1 RX 512KiB
slot1 TX 512KiB
...
```

显式 `--allow-any` 模式创建默认 4 个活跃 port slot。配置或 `--allow` 模式下，allowlist 同时决定允许注册的服务名和活跃 port 数量。每个 port 拥有一组 RX/TX；默认 4 slots 的 data plane 约 `4MiB`，6 slots 约 `6MiB`，64 slots 上限约 `64MiB`。

控制面保存活跃 port 数、channel 数、offset 和总映射大小。客户端按控制面映射 switch 拥有的对象，不假设编译期固定 data plane 大小。

## Switch 行为

`datarouter-switch` 是唯一创建和初始化 shared memory 的进程。它负责：

- 设置 shared memory 权限。
- 接受或拒绝服务注册。
- 将来源 TX channel 中的完整帧直接、原子地转发到目标 RX channel，不在热路径逐帧分配堆内存。
- 在配置的 TTL 后释放空闲 slot。
- 隔离损坏的 ring，并恢复死亡或超时的注册请求。

注册约束：

- 服务名语法必须合法。
- allowlist 不得为空；只有显式 `--allow-any` 才接受任意合法服务名。
- Linux 注册进程必须通过 PID、进程启动标识和真实 UID 检查，且 UID 必须与 shared memory owner 相同。
- 拒绝重复服务名。
- 不得超过活跃 port 数。

Switch 只读取 8 字节帧头，然后转发 payload 字节，不检查业务协议。

配置模式启动时只读取一次 INI。当前只接受大小写敏感的 `[common]` key：

- `allow_port=core,ml_manager`：配置模式必填。逗号分隔 token 会 trim，大小写敏感，必须唯一并符合服务名规则。
- `socket_raw=0|1`：保留 no-op flag。
- `socket_kcp=0|1`：保留 no-op flag。

未知 section 和 key，包括 `user` 这类未来字段，目前都会视为配置错误。

### Allowlist 与 TTL 建议

- **allowlist 必须包含所有实际注册的服务名**。当前 Core Launcher 拓扑为 `allow_port=core,cms,storage,portal,content,ml_manager`，并通过配置模式启动 switch，使活跃 port 数与服务数一致。漏列的服务名会导致注册时返回 `ERR_BUSY`。
- **TTL（`--ttl-ms`）**：switch 在 port 超过 TTL 无心跳后回收 slot。默认 30000ms。Content Manager 图片处理可能超过 3 秒，建议 TTL 不低于 30000ms。TTL 过短会导致长任务期间 port 被回收，后续请求返回 `NOT_FOUND`。
- **启动顺序**：datarouter 模式下，必须先启动 `datarouter-switch`，再启动 Core、Edge Node 或 Python worker。否则 `datarouter_open` 返回 `ERR_PLATFORM`。
- **权限与信任边界**：生产默认必须使用 `0600`，switch 与客户端由同一运行 UID 启动。更宽的 `--mode` 会输出醒目告警，只应用于受控调试。本加固边界把同一 UID 下的进程视为可信，不宣称能阻止同 UID 恶意进程直接改写 shared memory。

Frontend 默认限制单条完整消息为 `64MiB`、同时重组组数为 `1024`、总重组内存为 `128MiB`，并按 `chunk_ttl_ms`（默认 30 秒）回收不完整消息。分块 ACK 会绑定预期来源；最后一块只有在完整消息成功进入应用队列后才确认。`MAX_APP_PAYLOAD` 保持 `60KiB`，因此大消息仍按小帧分块，而不是使用整条 512KiB ring 作为单帧。

## Port 行为

Port 库是应用面对的 ABI。它打开 switch-owned shared memory，调用 `datarouter_logon` 注册服务名，将 outbound frame 写入 TX，再从 RX drain inbound frame。

`datarouter_send(core, payload)` 的实际含义是：调用者把一帧写入自己的 TX channel，帧头目标 route id 指向 `core`。Switch 稍后把这帧移动到 `core` 的 RX channel，并把 route id 改写为来源 port。

当前版本是基于 signal + 轮询的通知模型；文档和代码中的 interrupt 表示逻辑投递事件，不是 OS signal，也不是强制阻塞唤醒原语。

## 部署

### 布局升级

当前 shared-memory layout 9、8 字节帧头、26 位 route generation 以及 frontend ABI v2 与旧版本不兼容。升级时必须停止所有 frontend 和 switch，替换 switch、C/Rust 库及 Go/Python 绑定，删除旧 layout 的 shared-memory object，再整体启动。Linux 默认对象 `/datarouter` 对应 `/dev/shm/datarouter`。不要混跑新旧客户端；旧 frontend open/stats 符号只安全返回 `DATAROUTER_ERR_PROTOCOL`，v2 符号会先校验 struct size 与 ABI version，shared-memory 布局不一致同样明确返回 protocol mismatch。

### 启动顺序

datarouter 模式下，**必须先启动 `datarouter-switch`**，再启动 Core、Edge Node 或 Python worker。否则 `datarouter_open` 返回 `ERR_PLATFORM`。

### 手动启动

```bash
# 1. 先启动 switch
./scripts/start-switch.sh &

# 本地调试如 root 启动 switch、普通用户启动 portal：
# ./build/datarouter-switch --mode 0666 --config /path/to/switch.ini &

# 2. 再启动 Core
cd core && go run . &

# 3. 启动 Python worker（如 content_manager）
cd backend/cms/workers/content_manager && python3 main.py
```

### systemd

```bash
# 安装 switch service
sudo cp scripts/blinkcms-datarouter-switch.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now blinkcms-datarouter-switch

# 然后启动其他服务
sudo systemctl start blinkcms-core
```

`blinkcms-datarouter-switch.service` 声明 `Before=blinkcms-edge-node.service`，确保 switch 在 Edge Node 之前启动。Core 和 Python worker 的 systemd unit 应添加 `After=blinkcms-datarouter-switch.service` 和 `Wants=blinkcms-datarouter-switch.service`。
