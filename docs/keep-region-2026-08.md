# DataRouter KEEP 区域设计（2026-08）

> 关联：`../docs/database-keep-report-2026-08.md`（纲领）、database 仓库
> `docs/architecture.md`（L1 消费侧）。
>
> 本设计将 datarouter 共享内存布局升级为 **layout 13**，新增由 Switch 维持的
> KEEP 区域，并下沉 wire 校验到 C。layout 升级是热路径/ABI 变更，落地后必须
> 重跑全量基准（约束：datarouter 变更后跑分）。

## 1. 目标与非目标

**目标**
- 供 database（L1 缓存）与可读服务（portal 等）共享**只强调一致性、高频命中的查询结果缓存**。
- 命中路径**不经 Ring Buffer、不经 switch 路由**：调用方在其对同一共享内存的映射中直接完成
  lookup + 读取（`get_ref` 零拷贝，`get` 一次 memcpy）。
- 写入受控：仅 **core** 与 **core 授权的服务**可写；其余服务只读。授权经 Switch 控制面完成，
  位图只能由 Switch 改。
- 区域由 Switch 维持：初始化、容量上限（≤ 8 MiB，可配）、统计、淘汰观测量都在 Switch 侧面收敛。

**非目标**
- 不做业务解析：key/value 对 datarouter 均为**不透明字节**；fingerprint 只用于槽位定位，
  精确比较仍比对 key 字节。
- 不做对抗安全：与现有 registry/ring 的授权模型一致（协作式 + 进程身份校验）。
- 不做 L2/L3 语义：那属于 database 仓库（进程内 LRU / Redis）。

## 2. 内存布局

```text
0 ┌──────────────────────────────────────┐
  │ datarouter_control_plane（layout 13）│  ← 新增 keep 管理字段
  │  - keep_offset / keep_size           │
  │  - keep_write_allow_bitmap (uint64)  │  ← bit i = slot i 可写；仅 Switch 可改
  ├──────────────────────────────────────┤  ─ data_offset（与 layout 12 相同）
  │ channels: 2×max_ports × (8 + CHAN)   │  ← 完全不变，存量偏移不越界
  ├──────────────────────────────────────┤  ─ keep_offset = data_offset + channel_blob
  │ KEEP 区域（≤ 8 MiB，默认 8 MiB）      │
  │  [ keep header ][ hash slots ][ arena ]│
  └──────────────────────────────────────┘  ─ total_size
```

- `keep_size == 0` 表示关闭（行为等于 layout 12 的语义，但 layout 版本仍是 13）。
- 旧二进制（layout 12）打开 layout 13 的 shm 会因 `datarouter_validate_shared_memory`
  拒绝（layout_version 不匹配），按要求重启协同进程即可（现有多进程重启语义）。
- `datarouter_shared_memory_size(max_ports, keep_size)` 由纯函数加入 keep 参数；
  保留单参旧签名作为 `keep_size=0` 包装以兼容既有测试与调用。

## 3. KEEP 内部结构

```c
#define DATAROUTER_KEEP_MAGIC           0x4B454550u          /* "KEEP" */
#define DATAROUTER_KEEP_LAYOUT_VERSION 1u
#define DATAROUTER_KEEP_DEFAULT_SIZE   (8u * 1024u * 1024u)  /* 8 MiB */
#define DATAROUTER_KEEP_KEY_MAX        256u
#define DATAROUTER_KEEP_VALUE_MAX      (DATAROUTER_KEEP_DEFAULT_SIZE / 4)
#define DATAROUTER_KEEP_SLOTS_PER_ENTRY 2u

typedef struct {
    uint32_t magic;
    uint32_t layout_version;
    uint32_t capacity;        /* arena 字节数 */
    uint32_t slot_capacity;   /* 哈希槽数 = 期望条目数 × 2（开放寻址负载 <50%） */
    uint32_t entry_count;     /* 存活条目 */
    uint64_t hits;            /* 统计 */
    uint64_t misses;
    uint64_t expired;
    uint64_t evictions;
    atomic_uint64_t generation; /* seqlock：写开始置奇，写结束置偶 */
    atomic_uint  writer_lock;   /* 写方串行化（CAS spin） */
} datarouter_keep_header;

typedef struct {
    uint64_t key_hash;      /* FNV-1a 64 指纹 */
    uint64_t expires_ns;    /* 绝对纳秒；0 = 永不过期 */
    uint32_t key_offset;    /* 指向 arena，-1U = 空槽 */
    uint32_t value_offset;
    uint32_t value_len;
    uint32_t age;           /* 命中/写入自增，LRU 淘汰依据 */
} datarouter_keep_slot;     /* 16-byte 对齐由编译器处理 */
```

**Arena 条目编码**（一条连续块）：

```text
[ u32 key_len ][ u32 value_len ][ key bytes ][ pad→4 ][ value bytes ]
              └──────────────── key_offset ──┘        └ value_offset ──┘
```

**查找**：`hash = fnv1a64(key)` → 开放寻址探测（线性，槽负载 <50%）→ 比较 key 字节 →
校验 `expires_ns` 与本地绝对时钟 → 命中/未命中。读写无需锁（见 seqlock）。

**写入协议（唯一写方并发串行化）**：
1. writer 获取 `writer_lock`（CAS）。
2. `generation` 置奇（表示写进行中）。
3. 定位/分配槽位 + arena 空间；空间不足先清过期、再逐出 `age` 最小者；单值 > `KEEP_VALUE_MAX` 拒绝（由 L2 承接）。
4. 写 key/value 字节、更新槽位。
5. `generation` 置偶、释放锁。

**读取协议（seqlock）**：
1. `g1 = generation`；`g1` 为奇 → 写进行中，重试。
2. 读取槽位 + 从 arena 复制 value。
3. `g2 = generation`；`g2 != g1` → 期间有写，重试（有限次）。
4. 重试超限 → 返回 `DATAROUTER_ERR_AGAIN`，调用方走 L2/L3。

每个端口的 `get_ref` 返回 `(指针, 长度, seq)`：指针直接位于该进程对共享内存的映射中，
配合上面协议，调用侧在应答前做一次 seq 复检即可零拷贝交付。

## 4. 权限模型

- `keep_write_allow_bitmap`：`bit i = 1` 表示 slot i（已注册服务）可写 KEEP。位图**只能由
  Switch 修改**（在 `control_lock` 临界区内）：
  1. **静态**：switch `--keep-write-allow core,database,...`，服务注册成功时若名字在名单中即置位。
  2. **运行时授权**：core 调 `datarouter_keep_grant(service)` → 向 switch 发
     `DATAROUTER_REG_OP_KEEP_GRANT` 控制请求 → switch 校验**请求者身份 = 已注册的 core** →
     找到目标服务 slot（未注册返回 NOT_FOUND）→ 置位。
- 每次 `keep_put` / `keep_invalidate` 在执行前校验：`port->slot_index` 的位已置 + 进程身份匹配
  当前注册（`datarouter_registry_metadata` 的 pid/uid/start_id 一致），避免复用 slot 的服务越权。
- `keep_get` / `keep_get_ref` / `keep_stats` 无权限要求（全读）。

## 5. C API（`include/datarouter.h`）

```c
typedef struct { uint32_t capacity, entry_count; uint64_t hits, misses, expired,
                 evictions, generation; } datarouter_keep_stats;

int datarouter_keep_get(
    datarouter_port *port,
    const void *key, size_t key_len,
    void *out, size_t out_cap, size_t *out_len);              /* 拷贝式 */

int datarouter_keep_get_ref(
    datarouter_port *port,
    const void *key, size_t key_len,
    const void **value_out, size_t *value_len_out,
    uint64_t *seq_out);                                       /* 零拷贝 */

int datarouter_keep_put(
    datarouter_port *port,
    const void *key, size_t key_len,
    const void *value, size_t value_len,
    uint64_t ttl_ns);                                         /* 0 = 永不过期 */

int datarouter_keep_invalidate(datarouter_port *port, const void *key, size_t key_len);
int datarouter_keep_invalidate_prefix(datarouter_port *port, const void *prefix, size_t prefix_len);
int datarouter_keep_stats(datarouter_port *port, datarouter_keep_stats *out);

/* core-only：授权另一已注册服务写入权限（走控制面给 switch，switch 校验请求者是 core） */
int datarouter_keep_grant(datarouter_port *port, const char *service_name);
```

- `switch` 增加 `--keep-size <bytes|[kKmMgG]>`（缺省 8m，`0` 关闭）与 `--keep-write-allow <csv>`。
- 控制面复用现有 `submit_control_request` 槽位机制，新增操作 `DATAROUTER_REG_OP_KEEP_GRANT=3`；
  请求字段与 REGISTER 共享（service_name 即目标），owner 字段携带请求者（core）身份。

## 6. Rust 前端暴露

- 新增 `src/frontend/keep.rs`：`Frontend::keep_get / keep_get_ref / keep_put / keep_invalidate /
  keep_invalidate_prefix / keep_grant / keep_stats`，包装 §5 的 FFI。
- 新增 `src/validation.rs`：Rust 侧唯一事实来源委托给 C（`datarouter_protocol_validate_tlvs` /
  `datarouter_protocol_validate_frame_header`），`protocol.rs` 的 `validate_tlvs` 改调之。
- `src/protocol_ffi.rs` 增加 keep 与 validation 的 `extern "C"` 导出（供 Java/JNI、Python、
  C#/C++ 复用）。

## 7. 校验规则（下沉 C）

`src/validation.c`（新增）：
- `datarouter_protocol_validate_tlvs(const uint8_t *data, size_t len, char *err, size_t err_len)`
  —— 与 `protocol.rs::validate_tlvs` 语义逐条等价：TLV 头 3 字节边界、u16 长度不越界、精确耗尽。
- `datarouter_protocol_validate_frame_header(const uint8_t hdr[8], size_t payload_len)`
  —— 帧头 type/flags/长度边界。
- 规则全部在 C，Rust 不再实现第二份规则（委托 FFI），避免双实现漂移。

## 8. 基准与验收

新增 `benches/keep_region.rs`（或等价 C bench，与 `benches/ipc_modes.rs` 同机同方法）评测：
1. `keep_get` 命中延迟（p50/p99）与吞吐（查/s）。
2. `keep_get_ref` 零拷贝命中延迟。
3. 端到端查询命中路径：portal 处理→KEEP 命中直返 vs 现有 ring 路由面（对照）。
4. `keep_put` 写入吞吐与淘汰压力（容量打满后的逐出开销）。

并用现有 `benches/ipc_frontend.rs` / `ipc_unsafe.rs` / `ipc_modes.rs` / `protocol_hot_path.rs`
做布局升级 + 校验下沉的回归对照。验收标准：协议热路径与数据面无回归（同机器 ±5% 内），
KEEP 命中延迟显著低于 ring 路由面全程延迟。

## 9. 落地清单

- [ ] `include/datarouter.h`：常量、统计结构、8 个 API 声明、`--keep-*` 文档注释。
- [ ] `src/datarouter_internal.h`：layout 13 字段、keep 类型、`DATAROUTER_REG_OP_KEEP_GRANT`。
- [ ] `src/keep.c`：init/put/get/get_ref/invalidate/prefix/stats/grant 请求构造 + FNV-1a。
- [ ] `src/validation.c`：TLV/frame 校验。
- [ ] `src/common.c`：size/init/validate 集成 keep 区。
- [ ] `src/switch.c`：`--keep-size`/`--keep-write-allow` 解析、初始化、grant 处理。
- [ ] `src/port.c`：`datarouter_logon` 成功后置静态名单位；keep 权限校验钩子。
- [ ] `src/frontend/keep.rs` + `src/validation.rs` + `protocol.rs` 委托 + `protocol_ffi.rs` 导出。
- [ ] `build.rs`：编译单元接入 `src/keep.c`、`src/validation.c`。
- [ ] `tests/keep_region.c` / Rust 集成测试：权限、过期、逐出、prefix 失效、撕裂读重试。
- [ ] `benches/keep_region.rs` + 全量基准复测 + `docs/` 基准报告更新。
- [ ] `README.md`：KEEP 小节 + 发布流水线覆盖式 publish 说明。