# 校验下沉（M1）基准：2026-08

对应里程碑：`docs/update-plan-2026-08.md` 的 M1（阶段 A 第一批下沉）。
变更：`src/validation.c` 新增 C 侧单一事实来源校验
（`datarouter_protocol_validate_tlvs` / `datarouter_protocol_frame_layout`），
Rust `protocol.rs` 相应经 FFI 委托（`src/validation.rs`），删除 Rust 内第二份
`validate_tlvs` 实现。

约束（AGENTS.md）：datarouter 变更后必须跑分，无回归（±5% 内），记录于
`docs/benchmark-*.md`。方法学（机器噪声等）沿用 `docs/benchmark-2026-08.md`。

## 1. 确定性基准（本仓库 `benches/`）

### 1.1 `protocol_hot_path`（改前后直接对照，纯 CPU、best-of-5）

| payload(字节) | 路径 | 改前 | 改后 |
|---|---:|---:|---:|
| 65536  | legacy（build_business_response+pack） | 4.51–4.62 ms | 4.40 ms |
| 65536  | direct（pack_business_response_into） | 2.41–2.55 ms | 2.39 ms |
| 524288 | legacy | 240–257 ms | 227 ms |
| 524288 | direct | 3.52–3.70 ms | 3.59 ms |

结论：直接测量被改路径（pack/校验）的确定性基准**无回归**（变化在噪声内，略偏提升）。

### 1.2 `validation_c`（新增，校验吞吐）

| 用例 | 耗时/op | 吞吐 |
|---|---:|---:|
| `validate_tlvs`（81B 业务块） | 5.69 ns | ~176 Mop/s |
| `raw_frame_layout`（65,625B 帧精确校验） | 8.03 ns | ~124 Mop/s |

（best-of-7；含自检断言。）

### 1.3 `ipc_unsafe`（直接数据面，未触碰该路径，改后采样）

| 场景 | 改前 | 改后 |
|---|---:|---:|
| 单程 1KiB | 517k msg/s / 505 MiB/s | 492k msg/s / 481 MiB/s |
| 单程 32KiB | 283k msg/s / 8855 MiB/s | 310k msg/s / 9688 MiB/s |
| 单程 300KiB | 61k msg/s / 17956 MiB/s | 63k msg/s / 18543 MiB/s |
| pingpong 64B p50 | 3.56 µs | 3.59 µs |

直接数据面吞吐/延迟在噪声带内持平（该路径不参与本次校验改动）。

## 2. 跨语言 org 套件（`CubeGardenEOS/datarouter-benchmark`）

- 本次构建全部 7 种语言（rust/c/cpp/go/python/csharp/java），链接重建后的
  `build/libblinkcms_datarouter.so`（含新校验符号），`DATAROUTER_DIR` 指向本次检出。
- 运行：`python3 run.py --languages rust,c,cpp,go,python,csharp,java`
  → `results/20260820-120702/`（results.json / summary.md / comparison.md）。
- 对比旧基线（`results/20260815-195844`）：同语言对在**同一轮内**出现 ±30–190% 的
  rt/s 抖动（例：rust↔rust 64B 同语言段 158.9k vs 跨语言段 214.1k rt/s；python 全项
  “提升” 12–196%）。跨进程/语言套件为**高噪声**测量，单轮差异无法归因于本次校验
  下沉；与既有 `docs/benchmark-2026-08.md` 记载的 ±30–60% 机器噪声一致。
- 结论：org 套件未显示**系统性**回归；p50 数据面延迟（c、rust↔rust 跨语言 4.0–4.1 µs）
  与基线一致。

## M1.5 补充（chunk/ack 编解码下沉 + 接收热路径优化）

- C 新增 `datarouter_protocol_chunk_metadata`（单遍校验+分类）/ `strip_chunk` /
  `ack_fields`；Rust `raw_is_single_frame`、`parse_chunk_metadata`、
  `strip_chunk_metadata`、`chunk_control_fields_from_var_data` 全部改 FFI 委托。
- 性能：接收热路径由「`raw_frame_layout`(C) + `validate_tlvs`(C FFI) + Rust 重扫 TLV」
  并为「`chunk_metadata` 单遍 C 分类」，消除一次冗余 FFI 调用与一次重复 TLV 扫描。
- 进一步：新增 `datarouter_protocol_classify_frame`，把帧头精确校验 + TLV 分类合并为
  **单次 FFI**（拷贝中性——只读已收到的借用字节，单帧仍 verbatim 入队 0 拷贝）；
  `inbound.rs::handle_raw_frame` 改用一个调用。
- 验收：
  - `protocol_hot_path`（决定性）无回归：64k direct 2.44ms / 512k direct 3.52ms。
  - `ipc_unsafe` 数据面持平：64B pingpong p50 3.56µs。
  - `validation_c` 新增接收分类指标：frame+single-frame 单遍分类 ~13.3 ns/op。
  - `validation_c`：`classify_frame` 单 FFI 分类 ~12.6 ns/op（不易量化的数以拷贝为主）。
  - org 套件（`results/20260820-122526/`）无系统性回归，err=0。
  - `cargo test` 73 通过（含 chunk 分类/strip/ack FFI 语义测试）。
  - **拷贝预算复核**：unsafe 直接数据面单程仍为 2 次拷贝（app→RX ring 1 次；
    RX ring→app buffer 1 次），环回每方向 2 次；本轮校验/分类仅作用于已读入的借用
    字节，未新增任何拷贝。

## 3. 验收结论

- 确定性基准（被改路径的直接度量）无回归 → 满足“±5%”硬门槛。
- 校验规则已成立 C 单一事实来源，Rust 仅 FFI 委托；`cargo test` 全绿（含新增
  `validation.rs` FFI 语义测试）。
- A3–A7 下沉为后续批次，不在本轮范围。

## 4. 复现

```bash
# 确定性基准
cargo build --release --bench protocol_hot_path --bench validation_c --bench ipc_unsafe
./target/release/deps/protocol_hot_path-*     # 或上述 validation_c / ipc_unsafe
# 跨语言 org 套件
cd ../datarouter-benchmark
DATAROUTER_DIR=../datarouter ./build.sh          # 全部语言
DATAROUTER_DIR=../datarouter python3 run.py
```
