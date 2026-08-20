# TODO：unsafe 直接数据面 → 缺省工作模式（临时目标清单）

状态：**unsafe 已是缺省工作模式（2026-08 翻转）**。crate 缺省开启 `unsafe` feature，
safe 拷贝模式经 `--no-default-features` 保留；ABI 兼容性（Rust/C/JNI/Python wheel）
已全链路验证（见「已完成（2026-08）」）。本清单剩余项是**后续阶段**的运行时
per-shm 模式强制（§1）与排他写者规则（§2）等。

## 1. safe 模式保留旧拷贝模式（不删除）

**crate 级已完成**：`--no-default-features`（`default-features = false`）即旧拷贝模式
（switch 路由，4 次拷贝），一键回滚；两个配置共享同一 layout 12 / C ABI（直接面只
多导出符号），`cargo test` 与 `cargo test --no-default-features` 均 68+ 通过。

**运行时 per-shm 全局模式（未实现，下一阶段）**：switch 启动参数 `--data-plane
routed|direct`（别名 `--safe-mode`）写入 shared memory 控制面（`data_plane_mode`
字段，layout 13）并在注册响应中回传；客户端设置项映射 + 校验 echo。语义：

- `routed`（safe）：只有拷贝数据面可用；`datarouter_unsafe_send` 返回
  `DATAROUTER_ERR_PERMISSION`（或新错误码），保证「safe 模式下不可能有直写」。
- `direct`（缺省）：直写为主，拷贝路径仍保留（switch 仍路由），但必须配合
  §2 的排他写者规则，否则混用即双写者竞态。
- 关键点：模式是 **per-shm 全局**（所有客户端一致），由 switch 强制，客户端只是
  声明意图并校验 switch 回传的模式，避免「一半 direct 一半 routed」的撕裂部署。
- 缺省值：crate 编译期缺省已是 direct；运行时开关与回滚路径待 layout 13 落地。
  届时同步处理 `tests/test_switch.c` 的既有 config 用例（见文末「已知的既有问题」）。

## 2. 混合模式排他写者规则（当前最危险的正确性缺口）

现状：混用时，同一目标 RX ring 可能同时被 switch 路由进程与多个直写进程写入——
ring 是 SPSC 设计，双写者会撕裂帧头/负载、触发 `corrupt_slot` 隔离风暴，甚至让
无辜的下一任占用者读到垃圾帧。现有测试只覆盖了「每 ring 单写者」的混合场景
（直发→正常 reader、正常发→直读 reader），掩盖了双写者。

候选设计：

- 方案 A（推荐，最小改动）：每个 slot 在注册时声明接收侧数据面偏好
  （`rx_direct: bool`，写入 registry，layout 12）：
  - 目标 `rx_direct=true`：只有直写者允许写该 RX ring；switch 路由遇到以它为目标的
    帧时**丢弃并计数**（或回退为 requeue），源端感知 NOT_FOUND 式背压。
  - 目标 `rx_direct=false`：`datarouter_unsafe_send` 拒绝（PERMISSION），只有 switch
    路由写该 ring。
- 方案 B：CAS 双头 ring（见 §3），从根上允许任意多写者，排他规则不再必要。
- 决策点：先做 A（小、可独立验证）还是直接上 B（大、但一劳永逸）。

## 3. 多写者串行化（解除「同一目标只允许一个逻辑写者」）

目标：允许多进程并发直写同一目标 RX ring。

- 方案 B1：每 slot 共享内存自旋锁（`direct_write_lock`），直写者持锁
  （memcpy 级临界区）写 ring；switch 路由不参与（由 §2 保证路由与直写不共存）。
  实现简单，但写者会互踩 cache line。
- 方案 B2：双头 ring（reserved head / published head）：写者 CAS 预留空间 →
  写 payload → 发布 published head；reader 只消费 published 以内的数据；容量判定
  用 reserved head vs tail。改动 ring.c 布局与所有读写路径，测试量大。
- 决策点：应用实际是「每服务单写者」模型（一个 portal 一个发送线程），B 的收益
  是消除文档约束；若 §2 方案 A 落地，B 可长期缓行。

## 4. 直写 pin 身份的残余问题

本阶段已把直写 pin 从 `io_users` 拆到 `direct_writers` + 写者身份三件套（layout
11），并让槽位释放「写者存活则等待、写者已死才清零」。残余限制：

- 身份三件套只有**最后一个**写者；若违反单写者约束并发直写，身份可能张冠李戴。
  依赖 §3 彻底解决。
- `datarouter_process_identity_matches` 在 hidepid / 权限受限环境可能把存活写者误判
  为已死（沿用现有 io_users 路径的既有语义，非本次引入）。
- `DATAROUTER_DIRECT_WRITER_WAIT_NS=500µs` 上限后返回 `0`，槽位留在 RELEASING 由
  cleanup 循环重试；写者 SIGSTOP 长时间暂停时槽位会滞留（优雅降级，不损坏数据）。

## 5. 部分汇编提升性能？—— 讨论结论（本阶段不实现，仅记录）

结论：**可行，但当前瓶颈不在指令选择，收益预期低；优先级低于 §2/§3 与下述
mask 化优化**。

依据（`datarouter-benchmark/docs/benchmark-unsafe-2026-08.md`）：

- 300KiB 单程已 18.2 GiB/s，接近该机 memcpy 带宽上限，属于带宽受限；汇编换指令
  换不来带宽。
- 小帧（≤1KiB）unsafe 与正常路径持平，瓶颈是单线程交替 send/recv 的流水线深度与
  原子/信号开销，也不是拷贝指令。
- 热循环 `ring_copy_in/out/between` 目前是编译器向量化的 `memcpy`；手写 asm 的
  常见收益点（movntdq 流式写避免污染 cache）在这里反而有害：接收端紧跟着读同一
  数据，非临时写会迫使接收端付出更慢的缓存未命中取回。

可做的低风险「接近汇编」优化（先测再上）：

1. **ring 容量改 2 的幂**：✅ **已完成（layout 12，2026-08）**。
   `DATAROUTER_RING_CAPACITY = DATAROUTER_CHANNEL_SIZE`（默认 2^19 = 524288），
   `ring_masked` 的 `% 524280`（真除法/libcall）变为 `& (CAP-1)`（单条 `and`，
   反汇编确认）；channel 磁盘布局为 8B 头 + 524288B 数据；同时给 C 库补上
   `-O3 -fno-strict-aliasing`（此前整个 C 库是 `-O0`）。
2. **AVX2 已实测，结论：不采用**。详见 `datarouter-benchmark/docs/benchmark-modes-2026-08.md`：
   手写 256-bit loadu/storeu 循环在 cache 热微基准里比 glibc memcpy 慢 19%，
   端到端 32KiB/64KiB 稳定落后，300KiB 持平；glibc 的 `__memmove_avx_unaligned_erms`
   本身即 AVX2 调优实现（scalar 模式下占 97% 周期）。`DATAROUTER_COPY=avx2`
   保留为实验开关，默认 scalar（libc memcpy）。
3. **-march=native / 手写 asm：不做**。大帧是内存带宽受限（cache 热 50 GiB/s
   vs 端到端 ~18.6 GiB/s），换指令选型无收益；小帧瓶颈在流水线深度与原子/信号
   开销。perf 剖面见 `datarouter-benchmark/docs/benchmark-modes-2026-08.md`。

**AVX-512 升级路径（已预留）**：ring.c 的拷贝分派已是策略表
（`kCopyStrategies`：scalar/avx2 + 每项 name/fn/supported 谓词）。未来若在
AVX-512 机器（Zen 4/Intel）上评估，只需新增 `copy_avx512` 内核
（`target("avx512f")`，64B loadu/storeu 循环）+ 一行表项，
`DATAROUTER_COPY=avx512` 即自动可用，无需改分派/基准/诊断代码。
前置条件不变：先确认 glibc 的 `__memmove_avx512` 不是已经在赢。

若未来要在异构 CPU 上再看拷贝，唯一值得的方向是**非临时写（movntdq）流式写**
——但接收端紧跟读同一数据，预期仍有害（2026-08 结论不变），仅当出现「发送端
写完即丢弃」的场景（如日志落盘代理）才值得重新评估。

## 6. 可观测性

- transport stats 增加：`direct_sends`、`direct_full`、`direct_pin_waits`、
  `direct_pin_force_releases`、`routed_frames_dropped_for_direct_target`（§2）。
- switch 启动日志与 `datarouter_transport_stats_snapshot` 暴露 `data_plane_mode`，
  客户端连接时校验并记录（模式漂移即告警）。
- registry snapshot 增加 `rx_direct` 标志，`registry_diagnostic` 展示。

## 7. 测试补强

- §2 双写者回归测试：同一目标同时直写 + 正常 send，断言不出现撕裂/协议风暴
  （在排他规则落地前，该测试应处于「预期可复现问题」状态，用作验收）。
- unsafe_send 与目标注销/崩溃回收并发（pin drain 路径）的压力测试。
- switch 重启期间 unsafe 收发（keepalive/自愈已有正常路径测试，补 unsafe 版）。
- 多线程 unsafe_recv 已加（`concurrent_unsafe_recv_never_torn_reads`）；补
  PAYLOAD_TOO_LARGE 后 frame_len 预取尺寸再收的成功路径测试。
- layout 11 的 C 侧 `test_layout.c` 已通过；新增「旧 layout 客户端拒绝」用例。

## 已完成（2026-08）

- layout 10 → 11：registry 槽位新增 `direct_writers` 计数器与写者身份三件套
  （pid/uid/process_start_id）。- `datarouter_unsafe_send` 直写 pin 从 `io_users` 拆分到 `direct_writers`（身份先存
  后增、seq_cst 握手）；`finalize_release`/`cleanup_releasing` 等待存活写者收尾，
  写者已死才清零，杜绝 ring 重置与在途 memcpy 竞态及 `io_users` 下溢。
- port 新增进程内 `rx_mutex`：直接模式多线程并发 `unsafe_recv`/`drain` 不再撕裂
  SPSC ring（仅在 `DATAROUTER_UNSAFE` 编译时加锁，正常路径零开销）。
- `Frontend::unsafe_recv` 修复：`PAYLOAD_TOO_LARGE` 不再被吞成超时，帧保留可重试；
  新增 `Frontend::frame_len()` 供预取帧长。
- 新增测试：并发 `unsafe_recv` 无撕裂、超尺寸帧保留并可换大缓冲重收。
- 全量测试：`cargo test`（68）与 `cargo test --features unsafe`（68+6）通过；
  `tests/test_layout.c` 通过。

### 缺省翻转与 ABI 兼容性验证（2026-08）

- **unsafe 成为缺省**：`Cargo.toml` `default = ["unsafe"]`；`--no-default-features`
  即旧拷贝模式（safe mode）。翻转不改变 layout（仍 12）/ C ABI（仍 v2），直接面只多
  导出符号。验证：`cargo test`（68+4+6）、`cargo test --no-default-features`（68+4）、
  C 侧 test_ring/test_config/test_layout 在有无 `-DDATAROUTER_UNSAFE` 下均通过。
- **ABI 钉扎测试**：新增 `tests/abi_compat.rs`（layout 12 常量 + frontend ABI v2 +
  C `version` 符号 + `copy_strategy` 缺省 scalar）；JNI 同步钉扎布局常量。
- **JNI 修复（历史漂移）**：`FrontendOptions` 初始化缺 `auth_token` 字段（auth 特性
  引入后 JNI 从未重新编译），补 `auth_token: None`；`cargo test`（jni/）通过，
  Java 侧 `javac --release 17` 编译通过。
- **Python 修复（历史漂移，由新测试抓出）**：`datarouter_frontend_stats` 的 ctypes
  镜像缺最后 3 个字段（`dead_process_reaps`/`transport_generation`/`switch_recoveries`），
  `stats_snapshot_v2` 因此返回 `PROTOCOL`；已补齐 `_lib.py` 与 `port.py`。
- **Python wheel 稳定性**：`setup.py` 强制平台标签 `py3-none-linux_x86_64`（原为
  `py3-none-any`，会跨平台错装）；新增 `python/tests/test_native_abi.py`（真实 .so
  ABI 交叉校验 + spawn 真实 switch 的端到端收发 + stats）；python 套件 34 用例全过；
  构建 wheel 后在全新 venv 安装并跑通全链路。
- **已知残余**：运行时 per-shm 模式强制（§1 layout 13）、§2 排他写者规则、§6 可观测性
  仍未实现（本清单后续阶段）。
- **C#/C++/Go 基准与绑定（2026-08）**：新增 `benches/csharp/`（dotnet 8 P/Invoke
  基准）+ `datarouter-benchmark/docs/benchmark-csharp-2026-08.md`；Go 包 `go/datarouter/`、C++ RAII
  `include/datarouter_frontend.hpp` + `tests/test_frontend_cpp.cpp`。实测 P/Invoke
  边界 ~12ns/次可忽略；**frontend 直接面（缺省）比裸 port API 还快**（64B 4.17 vs
  6.97µs），路由面是直接面 ~8×。选型建议：默认 frontend 直接面，低层 port API 为
  维护中的可选项。
- **frontend 缺省即直接数据面（本阶段，已关闭此前的 ABI 缺口）**：`Frontend::connect`
  / `datarouter_frontend_open_v2` 缺省打开直接面；`Frontend::connect_routed` /
  `open_v3 data_plane=ROUTED`（+ `auth_token`）显式切路由面；新增
  `datarouter_frontend_frame_len` C 导出；直接面下 `send`/`recv`/`recv_packed`/
  `frame_len` 直接读写 RX ring（≤ `MAX_APP_PAYLOAD` 单帧）。旧调用方零改动自动
  unsafe——现有 Rust/C#/Go/C++/Python/Java 绑定全部默认走直接面。验证：cargo test
  （68+4+11，两配置）、python 34 用例、Go 4 用例、C++ 11 断言、C# 基准、jni 全绿。

## 已知的既有问题（非本次引入，未修）

- `tests/test_switch.c` 的 `test_config_allowlist_and_capacity` 在 main 上即失败：
  测试以 5 端口 ini 启动 switch 却未传 `--max-ports 5`，switch.c（f02fecf 起）
  拒绝该配置。与 §1 safe 模式（switch 配置面）一起处理。
- 构建产物依赖：`tests/frontend.rs` 与 `tests/test_switch.c` 会拉起磁盘上的
  `build/datarouter-switch`；布局变更后必须用 `target/.../out/datarouter-switch`
  刷新该文件，否则出现 `DATAROUTER_ERR_PROTOCOL` 假失败。建议后续让测试直接用
  嵌入的 `DATAROUTER_SWITCH_BYTES` 或从 OUT_DIR 复制。
- python 测试套件（含新增 `test_native_abi.py`）需要真实 `.so`：源码树路径为
  `build/libblinkcms_datarouter.so`（`cargo build --release` 后按 `python/setup.py`
  的方式链接，或直接构建 wheel 安装后从 `_native/` 解析）；缺失时
  `test_protocol.py` 与 `test_native_abi.py` 都会失败（一致契约，非静默跳过）。
