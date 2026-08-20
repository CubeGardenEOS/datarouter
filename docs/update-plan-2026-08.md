# DataRouter 更新计划（2026-08）：先下沉特性，后增加 KEEP 区域

> 状态：计划（v1）。仅文档，待评审后再进入代码。
> 关联：`docs/keep-region-2026-08.md`（KEEP v2 设计）、database 仓库 `docs/report-2026-08.md`。
> 合规约束（见 CGEOS 根仓库 AGENTS.md「硬性工作约束」）：**datarouter 变更后必跑基准**；
> **校验规则一律下沉 C**；发布保持覆盖式 latest。

## 0. 为什么“先下沉、后 KEEP”

KEEP 区域的正确性依赖三块基础能力，它们目前分散在 Rust 前端：

1. **wire 规则**（TLV 边界、帧头校验）——属于“唯一事实来源”级规则，必须在 C 单点实现，
   否则 Rust/C/Java/Python 会对同一份协议各写一份校验，漂移不可避免。
2. **共享内存数据面操作**（帧头解析 → 路由解析 → 目标 slot 写、缓冲读取仲裁）——它们是
   与 shared-memory 布局强耦合的底层操作，应由 C 提供原语，Rust 只做委托与编排。
3. **性能底座**——先确认下沉后协议热路径与数据面无回归（±5%），再在其上叠加 KEEP，隔离
   “下沉引入的回归”与“KEEP 引入的回归”两种风险，便于定位与回滚。

因此更新分两个阶段，每阶段独立跑分、独立发版（覆盖式 latest）：

```
阶段 A（本计划 §1～§3）：前端特性下沉 C → 建立底层库能力 → 跑分（回归对照）
阶段 B（本计划 §4）：KEEP 区域（layout 13）→ 跑分（新增指标 + 回归对照）
```

## 1. 阶段 A：下沉前端特性到 C（作为平台能力）

### 1.1 下沉原则

- **下沉判据**：操作是「纯 wire 级 / 共享内存级、单一直观正确解、与进程内编排无关」→ 必须下沉。
- **保留在 Rust**：进程内并发编排（线程/通道/reader loop）、异步等待（ack waiter/future）、
  业务消息语义（模块/动作/请求ID 拼装）、生命周期与资源池调度决策、trace/timing。
- **委托模式**：每个下沉特性在 C 提供 `<prefix>_*` 原语，Rust 侧 `src/frontend/*.rs` 改为
  FFI 委托（`extern "C"`），**不复制第二份实现**；`protocol.rs::validate_tlvs` 为第一示范。

### 1.2 下沉批次清单

| # | 特性 | 现状（Rust） | 目标（C） | Rust 剩余职责 | 状态 |
|---|---|---|---|---|---|
| A1 | TLV 校验 | `protocol.rs::validate_tlvs` | `src/validation.c`：`datarouter_protocol_validate_tlvs`（已完成） | 仅 FFI 委托 + 错误码映射 | ✅ |
| A2 | 帧头解析 | `frontend.rs`/`unpack`/`raw_frame_layout` 内联 | `src/validation.c`：`datarouter_protocol_frame_layout`（已完成，含结构） | 组合与上层语义 | ✅ |
| A3 | 路由解析 | `frontend.rs::send` 内联查找 | **审计结论**：已下沉 —— `port.c` 的 `datarouter_send`/`unsafe_send` 内部经 `datarouter_registry_find_*` 完成 service 名→slot 解析；Rust 仅保留重试/回退编排（按设计原则留 Rust） | 调用方策略（超时/重试） | ✅ 审计 |
| A4 | 缓冲读取仲裁 | `frontend.rs::recv_packed`/`PortFrontend` | **审计结论**：已下沉 —— 环读取/帧头校验原语在 C（`datarouter_recv`/`unsafe_recv` + `frame_layout`）；仲裁/背压/队列仍在上层 | 队列/背压/超限语义仍在上层 | ✅ 审计 |
| A5 | Chunk/ACK 编解码 | `protocol.rs` 内联（`parse_chunk_metadata`/`strip_chunk_metadata`/`chunk_control_fields`） | `src/validation.c`：`datarouter_protocol_chunk_metadata` / `strip_chunk` / `ack_fields`（已完成，单遍分类） | ack 等待表、超时判定（Rust） | ✅ |
| A6 | TLV builder 原语 | `protocol.rs::VarDataBuilder` / `tlv_read_*` + `protocol_ffi.rs` | 既有 C ABI（`datarouter_tlv_put_*`/`read_*`）实现下沉为 C（重构） | 类型化消息门面（保持 ergonomic） | ⏳ 单独评审 |
| A7 | 诊断快照 | `registry_diagnostic` | **审计结论**：数据图已下沉（`datarouter_registry_snapshot`）；文本渲染留绑定层 | 格式化渲染 | ✅ 审计 |

> 说明：M1（第一批下沉）＝ A1 + A2；M1.5（本轮）＝ A5（chunk/ack 编解码单遍 C 实现）+
> A3/A4/A7 审计（确认路由、缓冲读取原语、诊断快照已在 C，Rust 仅编排/渲染）+
> 接收热路径性能优化（`raw_is_single_frame` 由「validate+重扫」并为一趟 C 分类，消除一次
> 冗余 FFI 与一次重复 TLV 扫描）。A6（TLV builder 原生下沉）跨所有绑定、工作量较大，
> 按用户意见**单独评审**后再实施。
### 1.3 阶段 A 落成物

- `src/validation.c`、`src/frontend.c`（或并入 `port.c` 的原语组）、`include/datarouter.h` 新增
  声明；`build.rs` 编译单元接入。
- `src/frontend/*.rs` 改委托；删除 Rust 侧重复实现（A1 必须删除，其余按批次收敛）。
- FFI 导出对齐：`protocol_ffi.rs` 增加 A2/A5/A6 导出（Java/JNI、Python、C# 同步可用）。
- 阶段 A 单元测试：与既有 `tests/*` 逐条语义等价（把 `protocol.rs` 行为测试平移为 C 侧 + 委托侧
  双跑一遍）。

### 1.4 阶段 A 跑分（强制）

- 跑分集：`benches/ipc_frontend.rs` / `ipc_unsafe.rs` / `ipc_modes.rs` / `protocol_hot_path.rs`
  + 新增 `benches/validation_c.rs`（TLV/帧头校验纯函数吞吐）。
- 对照：同机两轮（改前 / 改后），记录 p50/p99/吞吐；**无回归（±5% 内）** 方可进入阶段 B。
  - 预期：A1/A5/A6 为纯计算，应持平；A4 由 Rust 委托 C 应持平或微升（去掉一层拷贝/校验重复）；
    任何超过 ±5% 的退化逐条分析并修回。
- 产出：更新 `docs/benchmark-*.md`（阶段 A 对照表）。

## 2. 阶段 B：KEEP 区域（layout 13）

- 完整设计见 `docs/keep-region-2026-08.md`（v2 修订）——含五项关键修订：
  1. Arena 改为 **Log-structured 环形分配器**（消除外部碎片，天然 FIFO/LRU，无 Compact）。
  2. 哈希表用 **Backward-shift Deletion**（无墓碑，无假性未命中）。
  3. 跨进程 Seqlock 明确 **acquire/release 屏障序列 + 写方崩溃自愈**（writer_pid + kill 探测 +
     Switch 接管恢复）。
  4. `get_ref` 零拷贝推出**两段式调用约定**（`keep_verify_ref` 交付前复检；禁跨异步持有）。
  5. `keep_invalidate_prefix` 扫描优化（空槽过滤 + 分片限流）。
- 阶段 B 依赖阶段 A：KEEP 的校验、帧头、路由解析直接复用已下沉原语；Rust 侧 `keep.rs` 只做委托。

### 2.1 阶段 B 跑分（强制）

- 新增 `benches/keep_region.rs`：命中延迟（p50/p99）、get_ref 零拷贝延迟、端到端 portal 直返
  对照、put 吞吐 + 环形打满 evict 开销、崩溃自愈路径损耗（正常路径零探测开销）。
- 重跑阶段 A 全量集做布局升级回归（±5%）。

## 3. 里程碑与验收

| 里程碑 | 内容 | 出口条件 | 状态 |
|---|---|---|---|
| M0 | 本计划评审通过 | 计划落库（本文档合入） | ✅ 评审通过 |
| M1 | 阶段 A 第一批下沉：A1 校验 + A2 帧头 → C（约束 2 强制项），Rust 委托 | 测试平移全绿；确定性跑分（`protocol_hot_path`）无回归；跨语言 org 套件在既有噪声带内 | ✅ 已落地 + 跑分 |
| M1.5 | 阶段 A 后续：A5 chunk/ack 编解码 → C（单遍分类）；A3/A4/A7 审计确认已下沉；接收热路径单遍优化 | 测试全绿；决定性跑分无回归；org 套件无系统性回归 | ✅ 已落地 + 跑分 |
| M2 | 阶段 B KEEP 代码（layout 13） | 集成测试全绿（含崩溃自愈/碰撞删除/重启重放）；跑分达标 | ⏳ |
| M3 | 全量回归 + 文档（README/KEEP/基准报告） | 覆盖式发布 latest；AGENTS.md 约束保持一致 | ⏳ |

- 每个里程碑独立发布（覆盖式 latest），发布流水线不保留历史版本（约束 1）。
- 桌面应用相关构建在本地流程完成前保持 CI 摘除（约束 3），本计划不引入任何桌面 job。

## 4. 风险与回滚

| 风险 | 缓解/回滚 |
|---|---|
| 下沉后某原语性能退化 | 阶段 A 独立跑分定位；原语可单独回滚到 Rust（委托处恢复内联），不影响 KEEP |
| KEEP 布局（layout 13）与旧进程不兼容 | 版本校验拒绝加载；按既有多进程重启语义协调重启 |
| 崩溃自愈路径本身有缺陷 | 写方崩溃测试 + ASan 跑；失败则 KEEP 关闭（--keep-size 0）退化为纯 L2/L3 |
| 跑分环境波动 | 同机两轮、同方法记录；判据 ±5% 为硬标准 |

## 5. 待办审批项

- [ ] 批准阶段 A 下沉批次清单（§1.2 顺序与范围）。
- [ ] 批准 KEEP v2 修订（`keep-region-2026-08.md`）中五项设计决策。
- [ ] 批准里程碑出口条件（§3）。