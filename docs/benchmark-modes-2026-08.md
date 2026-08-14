# 三模式性能对比 + mask 化 / AVX2 评估（2026-08）

机器：Ryzen 5 3600（Zen 2，无 AVX-512），WSL2（Fedora 44 内核 6.6.87.2）。
后台有 Gradle daemon 等噪声；每个模式跑 2 轮，表格列为各轮数值。
build.rs 本次起对 C 库加 `-O3 -fno-strict-aliasing`（此前为 `-O0`）。

## 结论速览

1. **mask 化有效且保留**：ring 容量改为 2 的幂（layout 12，数据区 524288 = 2^19），
   环形下标回绕从「按 524280 取模（非 2 的幂 → 除法/libcall）」变成单条
   `and $0x7ffff`。反汇编确认：`ring.o` 已无 div/umod，全部为 `and`。
2. **手写 AVX2 拷贝不采用**：在 cache 热微基准里比 glibc memcpy 慢 19%
   （ring write+read 300KiB：scalar 46.25 GiB/s vs avx2 37.54 GiB/s）；
   端到端 32KiB/64KiB 单程同样稳定落后（约 -10%~-22%），300KiB 持平（噪声内）。
   原因（perf record）：glibc 的 `__memmove_avx_unaligned_erms` 本身就是 AVX2
   调优代码，占 scalar 模式 97.3% 的周期；手写 256-bit loadu/storeu 循环无法
   胜过它。**保持默认 scalar（libc memcpy）**；`DATAROUTER_COPY=avx2` 仅作为
   实验开关保留（benchmark-only）。
3. **不引入 -march=native/手写 asm**：大帧路径是内存带宽受限（端到端 300KiB
   单程 ~18.6 GiB/s，而 cache 热 memcpy 微基准 50 GiB/s，差 2.7× 即真实内存
   流量），换指令选型换不来带宽；小帧瓶颈在单线程交替 send/recv 的流水线深度
   与原子/信号开销，也不是拷贝指令。perf 显示拷贝指令本身已占 97%，再优化
   拷贝实现的边际收益 ≈ 0。
4. **unsafe vs safe**（三模式对比，取两轮）：300KiB 单程 9.6×（18.7 GiB/s vs
   1.99 GiB/s），64B pingpong p50 7.1×（3.53µs vs 25.2µs），32KiB 单程 2.8×，
   64KiB 单程 ≈1.05×（大帧经 switch 时 ring_copy_between 是单次大拷贝，差距
   缩小）。

## 三模式对比表

### one-way 吞吐（portal → core，MiB/s，两轮）

| payload | safe(scalar) | unsafe(scalar) | unsafe(avx2) |
|---------|--------------|----------------|--------------|
| 1KiB    | 556          | 506 / 504      | 516 / 513    |
| 32KiB   | 3506         | 9590 / 10139   | 8555 / 7837  |
| 64KiB   | 12993        | 13489 / 13938  | 12266 / 11969|
| 300KiB  | 1992         | 18150 / 19338  | 18761 / 18869|

### pingpong p50（portal → core → portal，µs，两轮）

| payload | safe(scalar) | unsafe(scalar) | unsafe(avx2) |
|---------|--------------|----------------|--------------|
| 64B     | 25.2         | 3.60 / 3.53    | 3.52 / 3.54  |
| 1KiB    | 25.0         | 3.69 / 3.59    | 3.50 / 3.61  |
| 300KiB  | 300.8        | 29.8 / 29.8    | 31.4 / 31.0  |

注：1KiB 单程 unsafe 与 safe 持平（甚至略低）是 bench 单线程交替 send/recv、
unsafe 路径无 reader 预取的已知现象（与 2026-08 文档一致），不代表小帧无收益
（小帧 pingpong 仍有 7×）。

## perf 符号级剖面（/tmp/copybench，300KiB cache 热）

- scalar：`__memmove_avx_unaligned_erms` 97.29%，ring 逻辑 <0.3%。
- avx2：`copy_avx2` 76.29% + glibc memmove 21.42%。
- 结论：ring 下标/校验开销可忽略；拷贝实现（memcpy）决定一切；glibc memcpy
  已是最优实现，手写循环只会更差。

## 复现

```text
# 三模式端到端（每次独立进程，copy 策略进程级全局）
IPC_BENCH_MODE=safe   DATAROUTER_COPY=scalar cargo bench --bench ipc_modes
IPC_BENCH_MODE=unsafe DATAROUTER_COPY=scalar cargo bench --bench ipc_modes --features unsafe
IPC_BENCH_MODE=unsafe DATAROUTER_COPY=avx2   cargo bench --bench ipc_modes --features unsafe

# 拷贝原语微基准（cache 热）
cc -O3 -fno-strict-aliasing -DDATAROUTER_CHANNEL_SIZE=524288U -Iinclude -Isrc \
   -D_POSIX_C_SOURCE=200809L tests_bench_copy.c src/ring.c src/common.c src/port.c \
   src/registry.c src/scheduler.c src/signal.c src/router.c src/platform_linux.c \
   -o /tmp/copybench -lpthread
DATAROUTER_COPY=scalar /tmp/copybench
DATAROUTER_COPY=avx2   /tmp/copybench
```

## 变更清单（本次）

- build.rs：C 编译加 `-O3 -fno-strict-aliasing`（MSVC `/O2`）。
- layout 10/11 → 12：`DATAROUTER_RING_CAPACITY = DATAROUTER_CHANNEL_SIZE`
  （2 的幂，默认 524288）；channel 磁盘布局 = 8B 头 + 524288B 数据；新增
  幂次静态断言；`ring_masked` 用 `& (CAP-1)`。
- ring.c：拷贝策略分派 `DATAROUTER_COPY=scalar|avx2`（默认 scalar=libc memcpy；
  avx2 为实验开关，target("avx2") 属性隔离 + 非 x86-64 回退）。分派是策略表
  `kCopyStrategies`（name/fn/supported 谓词），**AVX-512 升级路径已预留**：
  未来加 `copy_avx512` 内核 + 一行表项即可，`DATAROUTER_COPY=avx512`
  自动可用；`datarouter_copy_strategy()` 诊断导出。
- lib.rs：`DATAROUTER_RING_CAPACITY = CHANNEL_SIZE`；新增 `copy_strategy()`。
- 新增 `benches/ipc_modes.rs`（safe/unsafe 双模式、tab 分隔输出、含解析后的
  copy 策略）与 `tests_bench_copy.c`（拷贝原语微基准）。
- test_layout.c 按新几何更新（channel = 头 + 数据，stride 用 sizeof）。
