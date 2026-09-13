# 安全改动后的源代码对照（2026-09-09）

结论：最新简化源安全检查 36/36 通过，但 PHP native 两项与稳态全矩阵仍失败；
未合入或更新消费者 DataRouter 指针。下方 2026-09-09 内容为历史对照，失败与无效尝试均保留。
## 当前简化源汇总（2026-09-11）

- `../datarouter-benchmark/results/security-simplified-20260911-configured-final/results.json`：
  `status=passed`，候选检查 **36/36**，C/runtime 清理 `remaining=0`；Windows
  Cargo target 使用本地 `%LOCALAPPDATA%\CGEOS\datarouter\target-datarouter-4b79a1601b94`。
- `../datarouter-benchmark/results/php-full-simplified-20260911-cached/verification.json`：
  功能 8/8、10243 项检查/请求、错误 0；benchmark 84 项、错误 0；native p99
  仍有 2 项失败：1024B `+15.270509%`、307200B `+26.556762%`。native
  baseline/candidate 是同一当前原生库、同一机器和同一参数的两轮控制语义，不是历史版本 baseline。
- `../datarouter-benchmark/results/steady-simplified-20260911-baseline-known-env/comparison.json`：
  `status=failed`、`correctness=false`、`matching_coverage=true`、
  `performance=false`；1440 项比较中 55 项 `regression=true`；绝对重复性超 5%
  为基线 **135/720**、候选 **98/720**。
- 基线 Go 的 4 个 `open sender: protocol error` 仍按历史记录保留；固定名称或旧
  shm snapshot 只是推断，未证实为根因。当前隔离改动已直接执行 CLI 所列出的 Go
  绑定测试命令，两轮 **4/4** 通过，package **1.028/1.031s** 且无 `dr-go` 残留。

安全门禁通过不代表性能/PHP 门禁通过；未满足全部门禁前不提交。

## 固定条件与记录

- 基线提交 `1990c840001a5f5effbc196ceb9f0d998a554135`；候选为该提交上的布局 14 安全修改。
- 同一主机、同一修复后的 benchmark 源码和进程管理实现，动态库路径显式选择对应源码产物。
- 基线、候选顺序执行，各两轮 360 个逻辑场景及额外 110 次 ACK RTT，当前契约下零失败。
- 两端固定 CPU 0、2；控制器限制在 CPU 4–23，排除端点的 SMT 同胞 1、3。
- 保留预热 10000 和既定稳态样本量；不裁剪样本，不挑选重跑结果。

## C 64 B pingpong

| 轮次 | 基线 p99 µs | 候选 p99 µs | 变化 |
|---|---:|---:|---:|
| 1 | 19.039 | 31.660 | +66.29% |
| 2 | 19.460 | 29.510 | +51.64% |

完整比较含 1440 项指标，其中 777 项超过 5% 回归门限；不能用其中改善的项目抵消失败项目。
重复性和前后性能分别记录在 results.json / comparison.json，不将重复性检查替代性能检查。

## 原始证据

原始目录（平级独立 benchmark 仓库）：
`/home/klp/CGEOS/datarouter-benchmark/results/security-source-comparison-20260909-c`。
`baseline/results.json`、`candidate/results.json` 记录编译参数、工具链、源码/二进制指纹、全部场景结果、线程亲和性与清理结果；
`comparison.json` 为逐指标前后比较；每个 `*.samples.json` 保留有序全部样本，可通过 tools/recompute.py 重算。

| 版本 | Switch SHA-256 | 共享库 SHA-256 |
|---|---|---|
| baseline | `d7da5f356c7ca0d7c456048a9abcc87644d954eac0326b026d18e2301a658a06` | `ae7dcd86964ef91807dd1ae6fb70e248f444257b128ce90d679e6512d6a82cc2` |
| candidate | `c5abb99bebe7ad86c39fb8a3fb2fec330c14fcbb416988df3d10bf8ca8f8a146` | `feed5f33a8038bab53b2de050f73a70de8c68f49e3183beef45eafed7067f805` |

两版本清理记录均为 remaining=0，各回收 1424 个进程组、475 个所属共享内存对象。

## 无效尝试也保留

- `security-source-comparison-20260909`：继承的 LD_LIBRARY_PATH 优先加载候选库，与旧 switch 布局不匹配；停止并保留失败日志。
- `security-source-comparison-20260909-b`：发现基线与候选会使用不同的进程管理代码，停止并保留记录。
- `security-source-comparison-20260909-c`：以上测量器问题修复后完整执行两轮；仍有超限性能回归。

历史 `+44.34%` 是另一套测量条件的记录，保持原样；本报告不以新数字解释或覆盖它。
下一步同二进制 A/A，再分别对监控、等待/通知和 Ring 做单因素 A/B/B/A。
扩展 sanitizer、故障和 Windows 编译结果见 security-audit-2026-09.md；未完成项仍阻止合入。

## 正式二进制的后续控制实验

已重建并核对 C benchmark SHA-256 与正式候选完全相同：
`4d51d5e38ba6ec5937600c075b60a9f9a486a4b0511ffef014b583b2bbbe2c8a`。
连同保存的原 switch/共享库，经 CLI diagnostic manifest 驱动：

- `security-original-aa-20260909`：p99 为 20.530/20.590 µs，变化 +0.29%，重复性通过。
- `security-original-monitor-20260909`：repeated/once/once/repeated 为
  20.870/20.800/20.360/20.359 µs，B/A 均值变化 -0.17%；A、B 组内重复性均通过。

这些实验固定三个二进制指纹及 CPU，监控实验只改变是否反复计算未保留的调度快照。
该因素未显示足以解释正式轮长尾的影响。正式轮 p99 回归继续保留，不能用本次较低数值解除门禁。
两组清理结果均为 remaining=0；manifest 与原始有序样本保存在 benchmark results 同名目录。
后续布局 15 增量包含注册引用状态机与 Rust KEEP 映射生命周期；本节不是其前后性能验收。
