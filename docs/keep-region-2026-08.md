# DataRouter KEEP 区域设计（2026-08，v2 修订）

> 关联：database 仓库 `docs/report-2026-08.md`（纲领）、`docs/architecture.md`（L1 消费侧）。
> 落地顺序见 `docs/update-plan-2026-08.md`（先下沉前端特性到 C，后实现 KEEP）。
>
> 本设计将 datarouter 共享内存布局升级为 **layout 13**，新增由 Switch 维持的
> KEEP 区域。layout 升级是热路径/ABI 变更，落地后必须重跑全量基准（约束：datarouter
> 变更后跑分）。KEEP 区域本身**必须叠加在已下沉的 C 底层能力之上**（见更新计划阶段 A）。

## 1. 目标与非目标

**目标**
- 供 database（L1 缓存）与可读服务（portal 等）共享**只强调一致性、高频命中的查询结果缓存**。
- 命中路径**不经 Ring Buffer、不经 switch 路由**：调用方在其对同一共享内存的映射中直接完成
  lookup + 读取（`get_ref` 零拷贝，`get` 一次 memcpy）。
- 写入受控：仅 **core** 与 **core 授权的服务**可写；其余服务只读。授权经 Switch 控制面完成，
  位图只能由 Switch 改。
- 区域由 Switch 维持：初始化、容量上限（≤ 8 MiB，可配）、统计、淘汰观测量都在 Switch 侧面收敛。
- **崩溃自愈**：任一写方（含 core 自身）在写过程中崩溃，读方向与后续写方向都必须能自愈而不是
  永久卡死（见 §3.4 崩溃恢复）。

**非目标**
- 不做业务解析：key/value 对 datarouter 均为**不透明字节**；fingerprint 只用于槽位定位，
  精确比较仍比对 key 字节。
- 不做对抗安全：与现有 registry/ring 的授权模型一致（协作式 + 进程身份校验）。
- 不做 L2/L3 语义：那属于 database 仓库（进程内 LRU / Redis）。
- 不做数据库语义：KEEP 只是 byte 缓存，无事务、无排序、无 Range Scan。

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

## 3. KEEP 内部结构（修订版）

### 3.1 头与槽位

```c
/* ---- ABI 守卫：编译期固定，MSVC/GCC/Clang 与 Rust FFI 必须逐字节一致 ---- */

#define DATAROUTER_KEEP_MAGIC           0x4B454550u          /* "KEEP" */
#define DATAROUTER_KEEP_LAYOUT_VERSION 2u                    /* 修订：arena v2 + 崩溃自愈 */
#define DATAROUTER_KEEP_DEFAULT_SIZE   (8u * 1024u * 1024u)  /* 8 MiB */
#define DATAROUTER_KEEP_KEY_MAX        256u
#define DATAROUTER_KEEP_VALUE_MAX      (DATAROUTER_KEEP_DEFAULT_SIZE / 4)
#define DATAROUTER_KEEP_SLOTS_PER_ENTRY 2u                   /* 开放寻址负载 <50% */

typedef struct {
    uint32_t magic;
    uint32_t layout_version;
    uint32_t capacity;        /* arena 有效字节数 */
    uint32_t slot_capacity;   /* 哈希槽数 = 期望条目数 × 2（开放寻址负载 <50%） */
    uint32_t entry_count;     /* 存活条目 */
    uint32_t pad0;
    uint64_t hits;
    uint64_t misses;
    uint64_t expired;
    uint64_t evictions;
    atomic_uint64_t generation;   /* seqlock：写开始置奇，写结束置偶，单调递增 */
    atomic_uint  writer_lock;     /* 写方串行化（CAS spin） */
    pid_t        writer_pid;      /* 持锁写方 pid；0 = 无写方（崩溃自愈用） */
    atomic_uint  recovery_seq;    /* Switch/接管者自愈后递增，幂等恢复计数 */
    uint32_t     _reserved[14];   /* 补足到 128B（% 64 == 0，对齐 8） */
} datarouter_keep_header;     /* 128B，sizeof % 64 == 0，8 字节对齐 */

static_assert(sizeof(datarouter_keep_header) % 64 == 0, "keep header cache-line alignment");

typedef struct {
    uint64_t key_hash;      /* FNV-1a 64 指纹（槽位定位 + 删除时反向位移判定） */
    uint64_t expires_ns;    /* 绝对纳秒；0 = 永不过期 */
    uint32_t key_offset;    /* 指向 arena 条目区；-1U = 空槽（无墓碑设计） */
    uint32_t value_offset;
    uint32_t value_len;
    uint32_t age;           /* 命中/写入自增，淘汰中仅作参考，主淘汰由 arena 控制 */
} datarouter_keep_slot;     /* 恰好 32 字节（static_assert 固定） */

static_assert(sizeof(datarouter_keep_slot) == 32, "datarouter_keep_slot ABI mismatch");
static_assert(offsetof(datarouter_keep_slot, key_hash) == 0
              && offsetof(datarouter_keep_slot, expires_ns) == 8
              && offsetof(datarouter_keep_slot, key_offset) == 16
              && offsetof(datarouter_keep_slot, value_offset) == 20
              && offsetof(datarouter_keep_slot, value_len) == 24
              && offsetof(datarouter_keep_slot, age) == 28,
              "datarouter_keep_slot field offsets");
```

- **不使用墓碑（tombstone）**：删除用 **Backward-shift Deletion**（见 §3.3），空槽唯一合法值
  是 `key_offset == -1U`，探测链不因空槽提前终止，无假性未命中（False Miss）。

### 3.2 Arena：Log-structured 环形分配器（v2 修订，替代朴素 Bump Pointer）

> 设计盲点修正：朴素 Bump Pointer（单调递增）在删除/过期后会在 Arena 中间留下空洞；若做
> Compact/Defrag 则需长时间持写锁并引发读方 Seqlock 重试风暴。v2 改用**环形（log-structured）
> 分配器**，无碎片、无 Compact、轻量，天然带 FIFO/LRU 淘汰语义。

**Arena 布局（单一环形缓冲区）**

```text
arena_base ───────────────────────────────────────── arena_base + capacity
           │  [ 条目 | 条目 | ...      ]           │
           └──── write_head ──────►  └── read/evict head 环绕 ──►
```

- **write_head**：新条目追加位置，单向递增，触顶环绕到 `arena_base`。
- **alloc 流程**：
  1. 假定本次需要 `need = ALIGN4(key_len + value_len) + 16` 字节。
  2. 若 `write_head + need > arena_base + capacity`，先尝试**清理头部**：从 evict_head 起把
     仍被槽位引用的条目**先置空其槽位**（本条目即为自己指向的这条，置空后该槽位允许被后续
     Backward-shift 重新填充），再推进 evict_head，直到腾出 need 字节或写探测计数到上限。
  3. 若清理头部后仍不足（单次分配超过可用环形空间），返回 `DATAROUTER_ERR_FULL`，调用方
     （core/L2）降级走 L2/L3。
- **写入顺序（与 seqlock 配合）**：先把**新条目字节**写入 `write_head` 处（此时 generation 已
  置奇，读方不会接受），再更新槽位 offset/len/hash，最后 generation 置偶。被覆盖的旧条目若其
  槽位尚未被置空，则先置空槽位（见 §3.3 删除语义，避免残留槽位指向回写区域造成假命中）。

**淘汰策略（两层）**
1. **过期优先**：写入时若触发清理，优先回收 `expires_ns < now` 的头部条目（置空槽位 +
   `expired++`）。
2. **FIFO 其次**：无过期条目可回收时，按 evict_head 顺序逐出最旧条目（置空槽位 + `evictions++`）。
   `age` 字段**不参与 arena 主淘汰**，仅作为 switch 观测/统计维度；环形覆盖天然等价 LRU/FIFO
   混合，且零复制开销。
3. **写入前清理预算**：每次 `keep_put` 最多清理 `KEEP_MAX_EVICT_PER_PUT`（默认 64）个头部条目，
   超出即返回 FULL 由 L2 承接，避免单次 put 在锁内扫全量。

**备选方案（记录，不默认）**：若未来需要**值长度分布极不均匀 + 高并发小写**场景，可切换到
Slab/固定分桶（128B/512B/2KB/8KB/64KB 五类 + 每类 free list），代价是内存利用率下降与实现体积
上升。v2 默认环形，Slab 作为可插拔 arena 实现保留接口（`datarouter_keep_arena_ops`）。

### 3.3 哈希表：开放寻址 + Backward-shift Deletion（修订，消除假性未命中）

- 槽位 `key_offset == -1U` 即空槽；**无墓碑态**。
- **插入**：从理想位置 `h(key) % slot_capacity` 线性探测到空槽；负载 <50%，探测期望 <2。
- **查找**：线性探测，槽位 key_hash 快速过滤后精确比较 key 字节；遇空槽即终止（等价未命中）。
- **删除 `keep_invalidate` / 逐出（Backward-shift）**：

```c
static void slot_erase_shift(datarouter_keep_header *h, datarouter_keep_slot *slots,
                             uint32_t ci) {
    uint32_t cap = h->slot_capacity, next = ci;
    for (;;) {
        uint32_t j = (next + 1u) % cap;
        datarouter_keep_slot *s = &slots[j];
        if (s->key_offset == KEEP_EMPTY) break;            /* 簇结束 */
        uint32_t ideal = (uint32_t)(s->key_hash % cap);
        /* 仅当 s 的理想位置在 ci 与 j 之间（含 ci）时才有资格前移一位 */
        uint32_t dist_ideal = (j - ideal + cap) % cap;
        uint32_t dist_ci    = (j - ci + cap) % cap;
        if (dist_ideal >= dist_ci) break;                  /* 不越位，停止 */
        slots[ci] = *s;                                    /* 前移填补 */
        ci = j;                                            /* 空洞后移，继续 */
    }
    slots[ci].key_offset = KEEP_EMPTY;                     /* 尾部空洞置空 */
}
```

- 删除后不需要重新检查 `expires_ns` 期间的并发读：删除发生在**写端持有 writer_lock 且
  generation 置奇**期间，读方只会重试，不会读到半成品。
- 正确性测试（必须）：构造同簇哈希碰撞集 → 乱序插入 → 连续删除 → 断言全部剩余条目仍可按
  key 精确命中（无假性未命中）。

### 3.4 跨进程 Seqlock：内存序 + 崩溃恢复（修订）

**内存序（跨 x86 强序 / ARM64 弱序）**

- 写端（持 writer_lock 后）：
  1. `generation` 置奇。**置奇前**不写入任何共享数据；置奇后写 arena 字节。
  2. 写 arena 字节与槽位时按顺序进行，**全部写完成后**执行
     `atomic_thread_fence(memory_order_release)`。
  3. `generation` 置偶（`fetch_add(1)` 到偶数），再释放 writer_lock。
- 读端：
  1. `g1 = atomic_load_acquire(&generation)`；`g1 & 1` → 写进行中，重试。
  2. `atomic_thread_fence(memory_order_acquire)` 后读取槽位 + 从 arena 复制 value。
     （该 fence 保证：若看到后续偶数 generation，则此前发生的全部写入均对其可见。）
  3. `g2 = atomic_load_acquire(&generation)`；`g2 != g1` → 重试。
  4. 重试超限（默认 64 次）→ `DATAROUTER_ERR_AGAIN`，调用方降级 L2/L3。
- 判定成功的充分条件：`g1 == g2` 且 `g1` 为偶。因 generation 每次写单调 +2 环绕，任何
  交错都不会出现“读到半写状态却通过校验”的情形。

**崩溃恢复（关键新增）**

- `writer_lock` 持有者同时记录 `writer_pid`。写方每步更新字段前把自身 pid 写入 `writer_pid`。
- 探测：读方重试超限 或 写方 CAS 自旋超过 `KEEP_LOCK_SPIN_LIMIT`（默认 50k 次）时，
  执行 `kill(writer_pid, 0)` 存活探测（`datarouter_pid_alive`，common.c 已有）：
  - 存活 → 判定为“长写”（值过大/清理过重），继续自旋或返回 AGAIN（不抢锁，避免双写冲突）。
  - 已死 → **接管者**（任一写方均可，或以 `recovery_seq` 仲裁）CAS 置空 `writer_lock` 与
    `writer_pid`，`generation` 直接写为下一偶数（`(g | 1) + 1`，确保读方重试能越过奇数期），
    `recovery_seq++`；恢复后可继续提供写服务。Switch 侧观测到 `recovery_seq` 变化记录告警。
- 保证：**不会**出现因写方崩溃而永久锁死；**不会**有两个接管者同时写入（CAS 仲裁 + 奇偶
  generation 校验）。
- 集成测试（必须）：在 generation 为奇数时 `kill -9` 写进程 → 断言读方在超限后进入
  自愈路径并最终回到可用状态，且无内存非法访问（ASan/UBSan 跑一遍）。

### 3.5 `get_ref` 零拷贝 API 的调用约定（修订，强制文档化）

```c
/* 正确模式：本地复制/发送完成前必须维持 generation 校验 */
const void *val = NULL;
size_t len = 0;
uint64_t seq = 0;
if (datarouter_keep_get_ref(port, key, klen, &val, &len, &seq) == 0) {
    send_to_socket(val, len);            /* 或 memcpy 到应答包 */
    if (!datarouter_keep_verify_ref(port, seq)) {
        /* 数据发生撕裂/并发修改：丢弃应答并重试/走降级 */
    }
}
```

- `datarouter_keep_get_ref` 返回：`(g1, 槽位+arena 指针)`；`datarouter_keep_verify_ref` 再次读
  `g2` 并断言 `g1 == g2` 且为偶。**等价于**“先开放读取、交付前复检”的两段式 seqlock。
- **严禁**：调用方把 `val` 指针跨异步 await、存入结构体长期持有、或在 `verify` 通过之后才进入
  异步段。共享内存随 switch 生命周期存续，但内容不保证；长期引用一律复制后使用。
- `datarouter_keep_get`（拷贝式）内部自行完成两段校验，只对调用方返回一份稳定拷贝，无上述限制。
- Java/JNI、Python、C# 侧通过 `protocol_ffi` 的 C 导出复用同一约定，不回传裸指针给 GC 语言。

### 3.6 `keep_invalidate_prefix` 扫描优化（修订）

- 8 MiB 下槽位数可上万；全量前缀比较在持锁期间代价高。优化顺序：
  1. **空槽过滤**：先检查 `key_offset != KEEP_EMPTY` + `key_hash != 0`，空槽不解引用 arena，
     避免 Cache Miss 与无意义比较。
  2. **字节比较最小化**：先比前缀长度 + 前若干字节（可对齐读 u64），命中前缀段差异即跳过。
  3. **清理预算限流**：每次调用最多处理 `KEEP_MAX_PREFIX_SCAN`（默认 4096）个有效槽位；超出
     剩余部分返回部分完成（`DATAROUTER_ERR_AGAIN`），调用方（core 变更链路）分片重试，避免
     单个写锁长占。
  4. 不引入额外索引（省内存）；统计 `prefix_scan_slots / prefix_scans` 供 switch 观测，若扫描
     压力大再考虑给 key 前 8 字节维护副指纹加速。
- 语义：前缀匹配按字节前缀相等判定；`prefix_len == 0` 视为全清（慎用，与单独 full-flush API
  合并）。

## 4. 权限模型

- `keep_write_allow_bitmap`：`bit i = 1` 表示 slot i（已注册服务）可写 KEEP。位图**只能由
  Switch 修改**（在 `control_lock` 临界区内）：
  1. **静态**：switch `--keep-write-allow core,database,...`，服务注册成功时若名字在名单中即置位。
  2. **运行时授权**：core 调 `datarouter_keep_grant(service)` → 向 switch 发
     `DATAROUTER_REG_OP_KEEP_GRANT` 控制请求 → switch 校验**请求者身份 = 已注册的 core** →
     找到目标服务 slot（未注册返回 NOT_FOUND）→ 置位。
  3. **Switch 重启恢复**：Switch 重启重建共享内存时位图清零；core 在 `datarouter_logon`
     重连成功后，**自动重放全部静态名单**（`--keep-write-allow` 由 switch 留存配置）+ 补发
     runtime grant（core 本地保存其授权过的服务清单并按需重发）。测试场景必含：写 switch 重启
     → 各类服务挂载回调重新触发 grant → 断言位图与授权清单一致。
- 每次 `keep_put` / `keep_invalidate` / `keep_grant` 在执行前校验：`port->slot_index` 的位已置 +
  进程身份匹配当前注册（`datarouter_registry_metadata` 的 pid/uid/start_id 一致），避免复用
  slot 的服务越权。
- `keep_get` / `keep_get_ref` / `keep_stats` 无权限要求（全读）。

## 5. C API（`include/datarouter.h`）

```c
typedef struct { uint32_t capacity, entry_count; uint64_t hits, misses, expired,
                 evictions, generation; uint32_t recovery_seq; } datarouter_keep_stats;

int datarouter_keep_get(
    datarouter_port *port,
    const void *key, size_t key_len,
    void *out, size_t out_cap, size_t *out_len);              /* 拷贝式 */

int datarouter_keep_get_ref(
    datarouter_port *port,
    const void *key, size_t key_len,
    const void **value_out, size_t *value_len_out,
    uint64_t *seq_out);                                       /* 零拷贝两段式 */

int datarouter_keep_verify_ref(
    datarouter_port *port, uint64_t seq);                     /* 交付前复检 */

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
- 错误码复用：`ERR_FULL`（arena 空间不足走 L2/L3）、`ERR_AGAIN`（读写重试超限/扫描部分完成）、
  `ERR_NOT_FOUND`（未命中或未授权目标）、`ERR_PERMISSION`（越权写）、`ERR_INTERNAL`（自愈接管冲突）。

## 6. Rust 前端暴露

- 新增 `src/frontend/keep.rs`：`Frontend::keep_get / keep_get_ref / keep_verify_ref / keep_put /
  keep_invalidate / keep_invalidate_prefix / keep_grant / keep_stats`，包装 §5 的 FFI。
- **Rust 侧只做委托**：`keep.rs` 不自行实现 hash/扫描/校验，全部调用 C 实现（单一事实来源）。
- `src/validation.rs`：Rust 侧唯一事实来源委托给 C（`datarouter_protocol_validate_tlvs` /
  `datarouter_protocol_validate_frame_header`），`protocol.rs` 的 `validate_tlvs` 改调之。
- `src/protocol_ffi.rs` 增加 keep 与 validation 的 `extern "C"` 导出（供 Java/JNI、Python、
  C#/C++ 复用；get_ref 对 GC 语言仅暴露拷贝式版本）。

## 7. 校验规则（下沉 C）

`src/validation.c`（已落地，M1）——**C 侧单一事实来源**，Rust 仅 FFI 委托：
- `int datarouter_protocol_validate_tlvs(const uint8_t *data, size_t len)`
  —— 返回 1 合法 / 0 非法；与 Rust 原 `validate_tlvs` 逐字节等价：3 字节头边界、
  u16 长度不越界、精确耗尽、长度无溢出；空块合法。
- `int datarouter_protocol_frame_layout(const uint8_t *data, size_t buf_len,
                                        datarouter_frame_layout *out)`
  —— 帧头解析 + 精确长度校验（buf_len == 8 + var_data + body），等价原
  `raw_frame_layout`/`unpack` 的精确匹配规则；`datarouter_frame_layout` 结构见
  `include/datarouter_protocol.h`。
- Rust 侧 `protocol.rs::validate_tlvs` / `raw_frame_layout` / `unpack` 均改用
  `src/validation.rs` FFI 委托（`protocol.rs::validate_tlvs` 不再有第二份实现）。
- 本项已随更新计划**阶段 A**（先于 KEEP）落地；A3–A7（路由/缓冲读取仲裁/
  Chunk-ACK/TLV builder 原生下沉）为后续批次，见 `docs/update-plan-2026-08.md`。

## 8. 基准与验收

新增 `benches/keep_region.rs`（或等价 C bench，与 `benches/ipc_modes.rs` 同机同方法）评测：
1. `keep_get` 命中延迟（p50/p99）与吞吐（查/s）。
2. `keep_get_ref` / `keep_verify_ref` 零拷贝命中延迟。
3. 端到端查询命中路径：portal 处理→KEEP 命中直返 vs 现有 ring 路由面（对照）。
4. `keep_put` 写入吞吐与淘汰压力（环形打满后的 evict 开销、FULL 降级比例）。
5. 崩溃自愈路径损耗：正常路径无 `kill(0)` 探测开销（仅重试超限才探测）。

并用现有 `benches/ipc_frontend.rs` / `ipc_unsafe.rs` / `ipc_modes.rs` / `protocol_hot_path.rs`
做布局升级 + 校验下沉的回归对照。验收标准：协议热路径与数据面无回归（同机器 ±5% 内），
KEEP 命中延迟显著低于 ring 路由面全程延迟；跑分**分布在阶段 A 与阶段 B 各一次**（约束 2）。

## 9. 落地清单（修订）

**阶段 A（先下沉，见 update-plan-2026-08.md）**
- [x] `src/validation.c`：TLV/帧头校验（`datarouter_protocol_validate_tlvs` /
      `datarouter_protocol_frame_layout`）；`protocol.rs` 相应委托、`unpack` 改调。
- [x] 阶段 A 全量基准回归：`protocol_hot_path` 无回归；跨语言 org 套件在既有
      ±30–60% 噪声带内。新增 `benches/validation_c.rs`（见
      `docs/benchmark-validation-2026-08.md`）。
- [ ] 前端下沉后续批次（路由/缓冲读取仲裁/Chunk-ACK/TLV builder 原语）由 C 提供，Rust 委托。

**阶段 B（KEEP 区域）**
- [ ] `include/datarouter.h`：常量、统计结构、9 个 API 声明、`--keep-*` 文档注释。
- [ ] `src/datarouter_internal.h`：layout 13 字段、keep 类型、`DATAROUTER_REG_OP_KEEP_GRANT`。
- [ ] `src/keep.c`：init/put/get/get_ref/verify_ref/invalidate/prefix/stats/grant 请求构造 +
      FNV-1a + **环形 arena + Backward-shift 删除 + seqlock 崩溃自愈**。
- [ ] `src/keep.c` 内 `static_assert` ABI 守卫；Rust `layout.rs` 侧同名 static_assert。
- [ ] `src/common.c`：size/init/validate 集成 keep 区（layout 13）。
- [ ] `src/switch.c`：`--keep-size`/`--keep-write-allow` 解析、初始化、grant 处理、
      崩溃接管观测（`recovery_seq` 告警）。
- [ ] `src/port.c`：`datarouter_logon` 成功后置静态名单位；keep 权限校验钩子；
      Switch 重启后 core 自动重放 grant 的接入点。
- [ ] `src/frontend/keep.rs` + `src/validation.rs` + `protocol.rs` 委托 + `protocol_ffi.rs` 导出。
- [ ] `build.rs`：编译单元接入 `src/keep.c`、`src/validation.c`。
- [ ] 集成测试：权限、过期、逐出、prefix 失效、撕裂读重试、
      **写方崩溃自愈（奇数 generation 强杀）**、
      **哈希碰撞 + 连续删除无假性未命中**、**Switch 重启 grant 重放**。
- [ ] `benches/keep_region.rs` + 全量基准复测 + `docs/` 基准报告更新。
- [ ] `README.md`：KEEP 小节 + 发布流水线覆盖式 publish 说明。

## 10. 风险与取舍

| 风险 | 缓解 |
|---|---|
| 环形 arena 覆盖旧条目时残留槽位指向回写区 | 写头推进前先置空被覆盖条目的槽位（§3.2 写入顺序） |
| 写方崩溃 → 永久锁死 | writer_pid 存活探测 + 接管者 CAS 恢复（§3.4） |
| ARM64 弱内存序下读方看到旧值 | 明确的 acquire/release 屏障序列 + 两段校验（§3.4） |
| Backward-shift 删除实现缺陷 → 假性未命中 | 碰撞+连续删除正确性测试 + ASan 测试（§3.3 / §9） |
| 8MiB 下 prefix 扫描长占写锁 | 空槽过滤 + 分片限流 + 部分完成语义（§3.6） |
| get_ref 裸指针被长期持有 | 调用约定强制 verify_ref + 文档化守卫（§3.5） |
| 多平台 ABI 偏移漂移（MSVC/GCC/Clang/Rust） | static_assert 双端守卫（§3.1 / §9） |