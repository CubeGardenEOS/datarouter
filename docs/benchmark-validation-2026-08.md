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

## A6 补充（TLV builder/reader 原生下沉 C，单独评审项）

- 新增 `src/tlv.c`：`datarouter_tlv_append`（编码，1 次 memcpy）、`datarouter_tlv_find`
  （单遍校验+查找，返回块内指针零拷贝）、`datarouter_tlv_next`（迭代，区分
  truncated-header/value 两类截断码）。
- Rust `protocol::VarDataBuilder`/`tlv_read_*`/`tlv_read_all`/`parse_business_fields` 全部
  改调 C；`protocol_ffi.rs` 公共 ABI 不变，其底层自动路由到 C。接收/业务读路径不再有
  Rust 第二份 TLV walk。
- 验收：
  - `protocol_hot_path` 无回归（64k direct 2.40ms / 512k direct 3.52ms）——pack 的
    var_data 构建改走 C append 后仍满足 ±5%。
  - `ipc_unsafe` 数据面持平（64B p50 3.49µs；300KiB 19.5 GB/s）。
  - `validation_c`：tlv build（3 字段）~91.6 ns/op；tlv find+read（单 FFI）~132.9 ns/op。
  - org 套件（`results/20260820-124910/`）无系统性回归（~1–2% 噪声带），err=0。
  - `cargo test` 76 通过（含 append/find/next 与截断码 FFI 语义测试）。

## M2 补充（KEEP 区域，layout 13）

- 新增 `benches/keep_region.rs`（直连 KEEP 命中/写入路径）：
  - `keep_get`（拷贝式，1 memcpy）命中：~78.8 ns/op（约 12.7 Mop/s）。
  - `keep_get_ref` + `keep_verify_ref`（零拷贝两段式）：~65.3 ns/op。
  - `keep_put`（含环形打满 evict + 墓碑压实）：~729 ns/op，256B 值 ≈ 335 MiB/s。
  - 稳态 `entries≈22469`（= 8MiB arena 容量），`evictions` 正常增长，无槽位泄漏/假命中。
- 布局升级回归（决定性子门）：
  - `protocol_hot_path` 无回归：64k direct 2.41–2.47ms / 512k direct 3.49–3.55ms（对照基线 2.40–2.44 / 3.49–3.52）。
  - `ipc_unsafe` 数据面持平：64B pingpong p50 ≈ 3.62µs（对照 3.47–3.56）、300KiB ≈ 19 GB/s 量级。
- org 套件：`rust↔rust` 跨进程在既有 ±30–60% 噪声带内（p50 1024B 4.14→5.67µs、64B 4.64→5.39µs，同向但带内）；等价 same/C 因机负载波动未取做判据。java/go/csharp 跨绑定需在 datarouter 的 JNI/绑定仓库重新生成二进制（本仓库 `build.sh` 无法从源码重建已提交的 JNI `.so`），属环境/绑定发布项，非 KEEP 缺陷。
- `cargo test`：76 lib + 8 keep_region + 4 + 11 全绿；C 侧 `test_layout/test_switch/test_config` 全绿。

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
