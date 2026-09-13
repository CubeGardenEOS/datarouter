# layout 18 小包缓存热路径实验

本轮已接入用户提供的授权业务服务器执行 CPU1/2/3 对照。阶段比较已完成，
最终重复对照未能确认 routed 小包收益，三阶段均不纳入主工作树；
全部实验代码保留在独立实验分支与冻结快照中，结果见后续章节。
不提交、发布、更新装配指针或替换业务产物。旧 ±5% 门禁不作为本轮一票否决规则。
本报告不代替八语言全量验收。

## 实验实现（RX / pointer / TLS 累计快照）

- 注册并重新验证映射后建立本进程 RX 队列头表与 TX 视图。每次选择核对
  缓存槽位与共享控制头的八个布局字段；不匹配报告损坏。来源逐个轮询，
  包含自发自收，使用递增和条件回绕；成功消费才推进游标。
- 空扫只读队列头，选中非空或损坏队列才计算 payload/ownership 地址。
  空快照无 payload 指针，调用方直接返回 AGAIN，不能重新读取 head 后
  使用该不完整视图；这覆盖空扫结束后写者恰好发布的竞态。
- 内部 ring 参数改为 `const datarouter_channel *`，游标校验共用
  `datarouter_ring_header_validate` C 内联实现。公共头文件、本轮共享布局、
  512KiB 容量、ownership、心跳、通知屏障与内存序均未修改。
- pthread key 继续负责租约析构，C TLS 指针只加速已有租约查询，保留 `getpid()`。
  fork、detach、析构和失效慢路径清除快捷指针；慢路径单独禁止内联，避免
  命中时分配拓扑/配额探测的大栈帧。Go 绑定及 OS 线程生命周期未改动。

## 冻结与证据

证据位于 `../datarouter-benchmark/results/hotpath-20260913/`（相对本仓库根）。
`baseline` 是任务开始时 layout18 的完整未提交源码快照，不是 Git HEAD 的替代检出。
HEAD 为 `4b79a1601b94de24100b01cb19f63a65756fc0f7`，必须结合源码 SHA256 识别产物。
`rx`、`pointer`、`tls` 分别冻结三个累计阶段。已有前序未提交改动全部保留。

- `baseline-manifest.json`、`baseline.diff`：修改前快照与原有变更。
- 各阶段 `build/manifest.json`：GCC 版本、完整编译命令、源码及产物指纹。
- `stage-sources.json`、`hotpath.patch`：阶段源码指纹与仅本任务的代码差异。
- `assembly-evidence.json`、各阶段 `build/*.asm`：对象与最终 ELF 反汇编。
- `benchmark/tools/verify_all.py`、`remote_pair.c`：CLI 入口执行器和前次相同探针。
- `local-abba/results.json`、`measurements.csv`、`assessment.json`：逐轮结果、
  p99、CPU 时间、角色亲和性、`stat`/`schedstat` 和清理证据。

稳定版使用前次取回的实际业务 Switch 与共享库，来自
`remote-stable-20260913/tested-binaries.tar.gz`；不把历史源码当作该产物的精确来源。
所有本地阶段以相同 `-O3 -fPIC -pthread` C 参数构建，构建结束才计时。
最初加入 `-Werror` 的优化构建在基线上即触发既有 Switch `stringop-overflow`
警告，随后恢复前次执行器参数，失败日志保留；独立静态检查仍使用严格参数。

## 功能与安全

`security-final/results.json`：42/42 通过。包含严格静态分析、ASan/UBSan、
ring/布局/协议/信号、线程租约、完整 Switch、Rust 测试、core/portal/cgeai
消费编译、Windows 编译、Cargo 打包及 Python sdist→wheel 构建与 ABI 检查。
Rust 各组共 102 项通过。新增注册后布局损坏检查和绕过 atfork 的原始 fork
PID 失效检查；原有公平性、并发接收、短缓冲、最大帧、环绕、混写、
注销复用、暂停/死亡写者和亲和性恢复检查继续通过。

`security` 与 `security-v2` 是主动中止的早期验证，保留失败及清理记录。
`security-v3` 为 41/42：系统 Python 缺少 build 包；最终使用已有
`~/.cache/datarouter-packaging-venv`，sanitizer 使用
`~/.cache/gcc-sanitizers/root/usr/lib64`，完整重跑通过。
安全检查两个执行器均记录 `remaining=0`，不是忽略失败后的摘要。

## 反汇编结论

修改前 `select_rx` 在共享库与 Switch 中各有两条 `div`，并调用
`datarouter_rx_channel` 和 `datarouter_ring_used`。RX 阶段后二者及除法从
正常扫描路径消失；剩余函数调用属于损坏上报分支。空扫循环为队列头指针
加载、head/tail 加载、比较与条件回绕，payload 地址计算在选中分支之后。
ring 校验原有 24B 参数从栈读取，指针阶段改为寄存器传入视图地址。
TLS 命中路径跳过 `pthread_once` / `pthread_getspecific`，仍调用 `getpid`；
隔离慢路径后入口栈帧由实验中间版本的 0x72c8 字节降为 0x18 字节。
这证明指令路径改变，不等于已经证明吞吐收益或 L1 命中率改善。

## 测量边界

本机 WSL2 有 SMT，CPU1 与 CPU0 同核、CPU2 与 CPU3 同核，因此本地补充
测量冻结 Switch/发送端/接收端为 CPU0/2/4（三个不同物理核），不是远端
CPU1/2/3 验收的替代。通过重编译的 `cgeos-cli` 在私有 PID/挂载命名空间中
执行，独立 tmpfs `/dev/shm`、每次唯一设备名；没有停止或修改业务实例。

每阶段 64B/1KiB、direct/routed、hybrid/spin，连续两组串行 A/B/B/A；
最终组合对修改前 layout18 和实际稳定产物补测 300KiB 与 10ms 空闲首帧。
预热 10000、正式 20000 RTT；空闲场景预热 100、正式 1000 RTT。
吞吐单位为完整往返/秒，不是单向帧/秒。p99 保留每轮值，不能将其均值
称作合并样本后的 p99。CPU 时间包含探针启动和预热，与前次探针口径一致。
本机没有 perf 可执行文件，未采集 L1 命中率及硬件计数器。

本机收益方向存在不一致，额外 A/B/B/A 结果一并保留，不挑选有利轮次。
以下本机结果保存首次实现时的状态；随后用户补齐了服务器连接信息，
远端测量与最终保留结论见本报告后续章节。

## 本机稳态结果（非远端验收）

两组 A/B/B/A 的完整 48 场景比较见证据目录 `comparison-table.md`；
每组 A、B 各两轮，正值表示 B 吞吐更高。下面保留关键的 routed 1KiB 数据：

| 比较 / 模式 | 第一组吞吐变化 | 第二组吞吐变化 | A 第一组两轮 p99 µs | B 第一组两轮 p99 µs | A 第二组两轮 p99 µs | B 第二组两轮 p99 µs |
|---|---:|---:|---|---|---|---|
| baseline-rx / hybrid | +7.92% | +1.64% | 26.229, 37.609 | 25.050, 22.760 | 19.359, 21.210 | 20.240, 21.280 |
| baseline-rx / spin | -1.35% | +1.64% | 31.059, 29.259 | 31.379, 32.369 | 29.989, 29.260 | 31.930, 29.100 |
| rx-pointer / hybrid | +4.97% | +9.06% | 33.640, 19.860 | 20.790, 20.620 | 32.039, 31.920 | 27.689, 20.150 |
| rx-pointer / spin | +0.19% | +4.28% | 25.919, 19.620 | 21.820, 22.819 | 29.800, 28.140 | 30.600, 19.080 |
| pointer-tls / hybrid | -7.01% | +1.40% | 19.400, 18.819 | 30.930, 28.560 | 32.219, 19.309 | 19.069, 28.820 |
| pointer-tls / spin | +1.46% | +4.88% | 30.679, 20.409 | 18.979, 29.730 | 28.469, 18.970 | 19.000, 18.590 |
| baseline-tls / hybrid | +3.48% | +2.54% | 32.649, 19.610 | 31.200, 19.100 | 19.429, 31.429 | 32.060, 19.369 |
| baseline-tls / spin | -10.18% | +2.92% | 29.740, 19.620 | 24.200, 24.599 | 28.649, 31.960 | 32.969, 29.909 |
| stable-tls / hybrid | -26.60% | -13.08% | 26.849, 18.190 | 54.950, 40.570 | 17.760, 30.459 | 30.639, 19.000 |
| stable-tls / spin | -8.34% | -9.08% | 31.889, 19.830 | 18.870, 19.930 | 29.350, 29.360 | 27.379, 19.140 |

组合相对 layout18 的 spin/routed/1KiB 为 −10.18% / +2.92%，两组方向相反；
相对稳定业务产物的 routed/1KiB 仍有 hybrid −26.60% / −13.08%、
spin −8.34% / −9.08% 的差距。本机结果不能证明缩小了原远端 11%–12% 的差距。
300KiB 也并非全面改善：相对 layout18 的 spin/direct 为 −1.86% / −2.42%，
相对稳定产物的 spin/routed 为 −11.10% / −5.89%。

因此本轮不将三个阶段宣布为最终保留组合；源码、补丁、冻结产物和所有
失败/波动结果以实验状态交付。后续不应以删除 ownership、屏障或生命周期
检查追分；心跳和调度缓存行争用仍属于独立后续工作。

## 空闲首帧与清理

本地最终完成 416/416 条有效记录（384 稳态、32 空闲），无帧校验失败。
所有记录至少有一个快照确认三个独立角色 CPU0/2/4，所有产物指纹测前测后相同。
工作树 src/tests/include 与冻结 TLS 阶段逐文件一致，公共头文件与修改前相同。
实际稳定库和 Switch 指纹与前次远端结果记录一致。

10ms 空闲场景的逐轮 p99 如下，A/B 各两次，单位 µs：

| 比较 / 模式 / 路径 | A 两轮 p99 | B 两轮 p99 |
|---|---|---|
| baseline-tls / hybrid / direct | 160.148, 155.347 | 162.557, 154.067 |
| baseline-tls / hybrid / routed | 272.256, 249.597 | 250.667, 262.426 |
| baseline-tls / spin / direct | 136.938, 78.259 | 130.248, 90.149 |
| baseline-tls / spin / routed | 133.868, 148.838 | 116.648, 165.557 |
| stable-tls / hybrid / direct | 200.887, 193.027 | 233.536, 225.797 |
| stable-tls / hybrid / routed | 241.447, 215.047 | 267.436, 268.087 |
| stable-tls / spin / direct | 215.687, 174.067 | 112.608, 100.558 |
| stable-tls / spin / routed | 192.907, 191.877 | 155.107, 136.748 |

空闲 p99 同样存在逐轮波动，仅保留实测，不能据此宣称已通过远端延迟门禁。
`cleanup.json` 确认本轮测试进程零残留，私有共享内存零残留；命名空间已退出，
宿主 `/dev/shm` 为空。当时尚未连接远端，未停止业务；该段只描述本地阶段。
远端测试与恢复证据另外保存在下节目录。

## 远端阶段对照

证据目录为 `hotpath-20260913/remote/`。服务器四个 vCPU 分别对应四个
独立客体核心，固定 Switch/发送端/接收端 CPU1/2/3。线上库及运行中的
memfd Switch 指纹与前次实际稳定产物完全一致。原缓存 CLI 已过时，
使用源码版本 `079684e4cc51b3c121827998bd5397109899dcc5` 的当前 CLI，
通过现有兼容 glibc loader 启动。四个阶段在服务器 GCC 11.5.0 上同参数构建，
所有构建结束后才停止业务并计时。

首次 `stages` 发现 cgroup CPU 控制器未启用，导致原有保守检查拒绝 spin；
主动中止并自动恢复业务，失败数据保留。修正测试单元 `CPUWeight=100`、
`CPUAccounting=yes` 后，父层及测试层 `cpu.max` 均为 `max 100000`，
不改程序、不伪造配额或绕过专用核检查。`stages-v2` 完成 32 项 C 检查与
192 条有效阶段记录，源码/产物指纹均未变化，所属进程及私有 shm 零残留。
该单元退出后业务自动恢复，再开启最终候选对照。

| 阶段 / 模式 / 路径 / 大小 | 第一组吞吐变化 | 第二组吞吐变化 |
|---|---:|---:|
| baseline-rx / hybrid / direct / 64 | -1.45% | +5.95% |
| baseline-rx / hybrid / direct / 1024 | +1.95% | +4.63% |
| baseline-rx / hybrid / routed / 64 | +0.43% | +12.95% |
| baseline-rx / hybrid / routed / 1024 | +1.40% | +18.22% |
| baseline-rx / spin / direct / 64 | -8.35% | +8.78% |
| baseline-rx / spin / direct / 1024 | +6.11% | +9.96% |
| baseline-rx / spin / routed / 64 | +8.62% | -3.07% |
| baseline-rx / spin / routed / 1024 | +4.26% | +3.01% |
| rx-pointer / hybrid / direct / 64 | -0.45% | +2.00% |
| rx-pointer / hybrid / direct / 1024 | -2.68% | -6.50% |
| rx-pointer / hybrid / routed / 64 | -10.14% | +19.25% |
| rx-pointer / hybrid / routed / 1024 | +2.87% | +17.37% |
| rx-pointer / spin / direct / 64 | -3.28% | +2.59% |
| rx-pointer / spin / direct / 1024 | +1.05% | -5.93% |
| rx-pointer / spin / routed / 64 | +9.84% | -11.31% |
| rx-pointer / spin / routed / 1024 | +0.21% | +8.90% |
| pointer-tls / hybrid / direct / 64 | -3.46% | +1.22% |
| pointer-tls / hybrid / direct / 1024 | -0.30% | +3.37% |
| pointer-tls / hybrid / routed / 64 | -14.17% | +5.17% |
| pointer-tls / hybrid / routed / 1024 | +5.96% | -6.48% |
| pointer-tls / spin / direct / 64 | +7.56% | +3.58% |
| pointer-tls / spin / direct / 1024 | +3.17% | +1.02% |
| pointer-tls / spin / routed / 64 | -0.74% | +1.72% |
| pointer-tls / spin / routed / 1024 | +0.22% | +6.13% |

阶段筛选：RX 的 1KiB 四种模式两组均改善，进入最终补测；64B 仍有方向波动，
不能宣称所有小包均改善。pointer 的 hybrid/direct/1KiB 连续 −2.68% / −6.50%，
64B routed 方向也反复；TLS 的 hybrid/routed/1KiB 为 +5.96% / −6.48%。
后两项没有足够稳定的收益证据，阶段筛选时已从工作树移除，保留完整累计快照及结果。
RX 工作树的独立 `security-rx/results.json` 再次 42/42 通过，清理 remaining=0。

## 最终稳态复验与保留决定

RX 独立候选的最终复验再次出现 routed 方向反转。相对修改前 layout18，
1KiB hybrid 为 −14.94% / +18.19%，spin 为 +9.80% / −8.57%。
这与阶段试验中较一致的 1KiB 改善不一致，无法确认优化收益。
相对实际稳定产物的 1KiB routed 仍为 hybrid −5.69% / −17.10%、
spin −15.33% / −16.59%，不能声称缩小了原 11%–12% 的差距。

因此不保留任何本轮运行时代码改动；主工作树 src/include/tests 已恢复到任务
开始时的逐文件内容，前序 layout18 开发改动完整保留。完整三阶段候选另存为
`experiment/cache-hotpath-20260913` 分支的独立未提交工作树：
`../datarouter-hotpath-experiment-20260913/`。它以原 HEAD 为基点并叠加冻结
源码，故 HEAD 不能单独标识实验；并未把前序未提交工作替用户提交。
`hotpath-20260913/remote/selection.json` 记录筛选原因及恢复文件列表。

这是按用户“额外 A/B/B/A 仍不能确认收益就不纳入”的规则做出的决定，
不是恢复旧 ±5% 门禁。direct 和部分大帧场景有收益，不足以证明本轮
routed 小包目标达成，也不会为追分删除 ownership、通知屏障或生命周期检查。

## 远端最终对照全表

`final-rx` 共 224 条有效记录（192 稳态、32 空闲），两组 A/B/B/A 的
吞吐变化如下。完整逐轮吞吐、p99、CPU 时间和调度/亲和性快照见
`remote/artifacts/final-rx/results.json` 与 `remote/measurements.csv`。
CSV 用 run 字段区分阶段复验与最终复验，避免同名场景混在一起。

| 比较 / 模式 / 路径 / 大小 | 第一组吞吐变化 | 第二组吞吐变化 |
|---|---:|---:|
| baseline-rx / hybrid / direct / 64 | +8.36% | +14.99% |
| baseline-rx / hybrid / direct / 1024 | +7.27% | -0.40% |
| baseline-rx / hybrid / direct / 307200 | -7.94% | +2.24% |
| baseline-rx / hybrid / routed / 64 | +2.77% | -11.78% |
| baseline-rx / hybrid / routed / 1024 | -14.94% | +18.19% |
| baseline-rx / hybrid / routed / 307200 | +5.07% | +2.77% |
| baseline-rx / spin / direct / 64 | +6.43% | +10.76% |
| baseline-rx / spin / direct / 1024 | +8.21% | +5.69% |
| baseline-rx / spin / direct / 307200 | +14.89% | -11.46% |
| baseline-rx / spin / routed / 64 | +16.59% | -7.88% |
| baseline-rx / spin / routed / 1024 | +9.80% | -8.57% |
| baseline-rx / spin / routed / 307200 | +3.34% | -1.23% |
| stable-rx / hybrid / direct / 64 | +13.24% | +23.08% |
| stable-rx / hybrid / direct / 1024 | +5.04% | +19.42% |
| stable-rx / hybrid / direct / 307200 | +30.88% | +26.52% |
| stable-rx / hybrid / routed / 64 | -7.99% | -6.12% |
| stable-rx / hybrid / routed / 1024 | -5.69% | -17.10% |
| stable-rx / hybrid / routed / 307200 | +22.36% | +31.09% |
| stable-rx / spin / direct / 64 | +1.45% | +11.97% |
| stable-rx / spin / direct / 1024 | +0.73% | +13.50% |
| stable-rx / spin / direct / 307200 | +21.65% | +23.06% |
| stable-rx / spin / routed / 64 | -17.32% | -1.06% |
| stable-rx / spin / routed / 1024 | -15.33% | -16.59% |
| stable-rx / spin / routed / 307200 | +24.09% | +54.83% |

关键 1KiB routed 的逐轮 p99（µs），每组分别列 A、B 各两轮：

| 比较 / 模式 | A 第一组 | B 第一组 | A 第二组 | B 第二组 |
|---|---|---|---|---|
| baseline-rx / hybrid | 62.663, 86.838 | 388.572, 73.829 | 116.620, 435.516 | 198.377, 112.694 |
| baseline-rx / spin | 53.817, 82.262 | 76.396, 52.897 | 51.881, 51.530 | 57.028, 71.270 |
| stable-rx / hybrid | 46.444, 48.920 | 58.384, 54.577 | 42.097, 46.588 | 139.663, 85.331 |
| stable-rx / spin | 48.341, 120.889 | 122.350, 114.102 | 39.154, 47.251 | 49.999, 88.363 |

10ms 空闲首帧逐轮 p99（µs）：

| 比较 / 模式 / 路径 | A 两轮 | B 两轮 |
|---|---|---|
| baseline-rx / hybrid / direct | 1845.673, 1407.965 | 1273.381, 1198.316 |
| baseline-rx / hybrid / routed | 1787.684, 2227.597 | 1978.422, 1905.399 |
| baseline-rx / spin / direct | 1132.954, 2002.865 | 3727.734, 1212.817 |
| baseline-rx / spin / routed | 3479.462, 4003.394 | 5004.127, 948.180 |
| stable-rx / hybrid / direct | 1378.182, 1491.619 | 846.588, 1127.096 |
| stable-rx / hybrid / routed | 2046.043, 2248.217 | 1964.815, 1741.960 |
| stable-rx / spin / direct | 1075.612, 1680.712 | 6007.117, 6015.571 |
| stable-rx / spin / routed | 1804.429, 1557.638 | 5876.781, 4996.085 |

## 硬件计数补充（独立诊断）

服务器 perf_event_open 支持 cycles、instructions、L1D read access/miss。
主对照完成并恢复业务后，使用独立 `remote_pair_pmu.c` 在相同 CPU1/2/3
分配下补跑 16 条 spin/routed 的 64B/1KiB A/B/B/A。该诊断不混入主吞吐表。
仅计两个端点的用户态，排除预热；Switch、内核与 hypervisor 不在计数范围内。
每个事件同时保存 raw、time_enabled、time_running，所有事件均全时运行，
没有 multiplexing。完整两端计数见 `remote/pmu.csv` 与 `artifacts/pmu/results.json`。

| 比较 / 产物 / 端点 / 大小 | cycles/RTT | instructions/RTT | L1 读未命中/RTT | L1 读未命中/访问 |
|---|---:|---:|---:|---:|
| baseline-rx / baseline / 发送端 / 64 | 24540.5 | 13932.7 | 83.87 | 2.366% |
| baseline-rx / rx / 发送端 / 64 | 20119.7 | 13813.0 | 87.06 | 2.289% |
| baseline-rx / baseline / 发送端 / 1024 | 35256.6 | 21329.2 | 147.88 | 2.726% |
| baseline-rx / rx / 发送端 / 1024 | 28401.4 | 18401.9 | 156.39 | 3.204% |
| stable-rx / stable / 发送端 / 64 | 12932.2 | 6562.1 | 80.24 | 4.758% |
| stable-rx / rx / 发送端 / 64 | 21223.9 | 15291.7 | 85.39 | 2.056% |
| stable-rx / stable / 发送端 / 1024 | 18658.9 | 7300.0 | 154.44 | 8.041% |
| stable-rx / rx / 发送端 / 1024 | 29672.7 | 19898.2 | 132.75 | 2.366% |

发送端 1KiB 对 layout18 的诊断中，指令/RTT 从 21329 降为 18402，
但 L1 读未命中/RTT 从 147.88 增为 156.39；不能据此声称 L1 未命中减少。
对稳定产物，RX 的未命中/访问比更低，但指令/RTT 约 19898 对 7300。
该比值受到总访问数影响，也没有覆盖 Switch，不能将较低比值解释为
整个 routed 路径更快或控制工作集已常驻 L1。忙等时长和客体调度同样会影响计数。

远端 GCC 11.5.0 的最终 ELF 反汇编也确认 baseline select_rx 含两条 div，
RX/pointer/TLS 各阶段为零。ring 指针传参与 TLS 快分支证据保存在
`remote/assembly-evidence.json` 和各阶段 `build/*.asm`，即使阶段被排除也不删除。

## 最终恢复与交付

远端成功记录：32 项 C 检查、416 条主对照、16 条独立 PMU 诊断。
各阶段及最终对照所有记录的帧校验、源码/产物不变检查、角色亲和性检查均通过；
性能方向不一致是本轮不保留优化的原因。首次缺 CPU 控制器的中止轮次另行保留，
不计入上述成功数量。原有本地 42 项安全检查和 RX 独立 42 项检查均已通过。

每个测试单元均使用私有 PID/挂载命名空间和独立 /dev/shm；退出 trap 与
ExecStopPost 均恢复 cgeos.service。最终候选单元耗时约 15 分 15 秒，
因大帧与空闲补测将临时超时上限由 12 分钟改为 20 分钟；临时 drop-in 已删除。
单位时间、异常中止和恢复过程保存在各单元 journal.log，不把 not-found 单元
查询的默认 Result=success 当作历史失败轮次通过的证据。

`remote/results.tar.gz` 已取回并核对全部测试产物，SHA256：
`5b4829b9b0e5c15d2a5a2c0540fa05ecc25bd577cbde68cfc7d8f5f133d11b6f`。
`remote/cleanup-final.json` 确认远端临时目录/归档、测试 cgroup 和测试共享内存
均已清理；CPU 控制器状态自动恢复为测试前的 `memory pids`。
CGEOS MainPID=79958，FRP MainPID=14996，均 active/running；运行中的
Switch 及生产共享库指纹与测试前一致，业务 cgroup 有 11 个进程。
本轮没有发布、替换线上产物、更新装配指针或提交前序开发改动。

最终交付为未采纳的实验代码、独立实验分支、完整反汇编与同机对照报告。
主工作树程序代码还原并重建，不把失败候选留作默认运行产物。
