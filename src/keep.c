/*
 * datarouter KEEP region — log-structured ring arena + open-addressing hash
 * with backward-shift deletion + cross-process seqlock with crash recovery.
 *
 * Single source of truth (C); the Rust frontend (`src/frontend/keep.rs`)
 * delegates here and MUST NOT carry a second implementation.
 *
 * Copy discipline: get (copying) does one memcpy (arena -> caller buffer);
 * get_ref returns a POINTER into the arena (zero-copy) with a seqlock epoch
 * that the caller must re-verify before delivering. The data plane's
 * 2-copy-per-direction budget is untouched: KEEP sits on the side, keyed by
 * opaque bytes.
 */

#include "datarouter_internal.h"
#include "datarouter_registry.h"

#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <process.h>
static int32_t keep_current_pid(void) { return (int32_t)_getpid(); }
#else
#include <unistd.h>
static int32_t keep_current_pid(void) { return (int32_t)getpid(); }
#endif

#define KEEP_ALIGN4(x) (((x) + 3u) & ~3u)

static uint64_t keep_hash(const void *key, size_t key_len) {
    uint64_t hash = 14695981039346656037ULL;
    const unsigned char *p = (const unsigned char *)key;
    for (size_t i = 0U; i < key_len; i++) {
        hash ^= (uint64_t)p[i];
        hash *= 1099511628211ULL;
    }
    return hash == 0ULL ? 1ULL : hash;
}

datarouter_keep_header *datarouter_keep_header_of(datarouter_shared_memory *shm) {
    if (!shm || shm->control.keep_size == 0u) {
        return NULL;
    }
    return (datarouter_keep_header *)((uint8_t *)shm + shm->control.keep_offset);
}

static datarouter_keep_slot *keep_slots(datarouter_keep_header *h) {
    return (datarouter_keep_slot *)((uint8_t *)h + DATAROUTER_KEEP_HEADER_SIZE);
}

static uint8_t *keep_arena(datarouter_keep_header *h) {
    return (uint8_t *)h + DATAROUTER_KEEP_HEADER_SIZE +
           (size_t)h->slot_capacity * DATAROUTER_KEEP_SLOT_SIZE;
}

void datarouter_keep_init(datarouter_shared_memory *shm, uint32_t slot_capacity) {
    if (!shm || shm->control.keep_size == 0u) {
        return;
    }
    datarouter_keep_header *h = datarouter_keep_header_of(shm);
    uint32_t keep_size = shm->control.keep_size;
    if (slot_capacity == 0u) {
        slot_capacity = keep_size / (DATAROUTER_KEEP_SLOT_SIZE * 4u); /* 槽位≈1/4 keep_size */
        if (slot_capacity < 32u) {
            slot_capacity = 32u;
        }
    }
    uint32_t slot_bytes = slot_capacity * DATAROUTER_KEEP_SLOT_SIZE;
    uint32_t capacity = keep_size - DATAROUTER_KEEP_HEADER_SIZE - slot_bytes;
    h->magic = DATAROUTER_KEEP_MAGIC;
    h->layout_version = DATAROUTER_KEEP_LAYOUT_VERSION;
    h->capacity = capacity;
    h->slot_capacity = slot_capacity;
    h->entry_count = 0u;
    h->used = 0u;
    h->hits = 0ULL;
    h->misses = 0ULL;
    h->expired = 0ULL;
    h->evictions = 0ULL;
    atomic_store_explicit(&h->generation, 0ULL, memory_order_release);
    atomic_store_explicit(&h->writer_lock, KEEP_WRITE_LOCK_FREE, memory_order_release);
    h->writer_pid = 0;
    atomic_store_explicit(&h->recovery_seq, 0u, memory_order_release);
    h->tombstones = 0u;
    h->write_head = 0u;
    h->evict_head = 0u;
    for (uint32_t i = 0u; i < slot_capacity; i++) {
        keep_slots(h)[i].key_offset = KEEP_EMPTY;
    }
}

/* ---- 崩溃自愈：探测持锁写方；若已死则由任一观察者接管 ---- */
static void keep_recover_if_dead(datarouter_keep_header *h) {
    if (atomic_load_explicit(&h->writer_lock, memory_order_acquire) == KEEP_WRITE_LOCK_FREE) {
        return;
    }
    int32_t wp = h->writer_pid;
    if (wp > 0 && datarouter_pid_alive(wp)) {
        return; /* 长写：不抢锁 */
    }
    unsigned int expected = KEEP_WRITE_LOCK_HELD;
    if (atomic_compare_exchange_strong_explicit(
            &h->writer_lock, &expected, KEEP_WRITE_LOCK_FREE,
            memory_order_acq_rel, memory_order_relaxed)) {
        uint64_t g = atomic_load_explicit(&h->generation, memory_order_relaxed);
        if ((g & 1ULL) != 0ULL) {
            atomic_store_explicit(&h->generation, (g | 1ULL) + 1ULL, memory_order_release);
        }
        atomic_fetch_add_explicit(&h->recovery_seq, 1u, memory_order_relaxed);
        h->writer_pid = 0;
    }
}

static int keep_writer_acquire(datarouter_keep_header *h) {
    uint32_t spins = 0u;
    for (;;) {
        unsigned int expected = KEEP_WRITE_LOCK_FREE;
        if (atomic_compare_exchange_strong_explicit(
                &h->writer_lock, &expected, KEEP_WRITE_LOCK_HELD,
                memory_order_acquire, memory_order_relaxed)) {
            h->writer_pid = keep_current_pid();
            uint64_t g = atomic_load_explicit(&h->generation, memory_order_relaxed);
            uint64_t odd = (g | 1ULL);
            if (odd == g) {
                odd += 2ULL;
            }
            atomic_store_explicit(&h->generation, odd, memory_order_release);
            return DATAROUTER_OK;
        }
        if (++spins >= DATAROUTER_KEEP_LOCK_SPIN_LIMIT) {
            keep_recover_if_dead(h);
            spins = 0u;
            if (atomic_load_explicit(&h->writer_pid, memory_order_acquire) > 0 &&
                datarouter_pid_alive(h->writer_pid)) {
                return DATAROUTER_ERR_AGAIN; /* 活的长写，放弃本次 */
            }
        }
        datarouter_yield();
    }
}

static void keep_writer_release(datarouter_keep_header *h) {
    uint64_t g = atomic_load_explicit(&h->generation, memory_order_relaxed);
    atomic_thread_fence(memory_order_release);
    atomic_store_explicit(&h->generation, g + 1ULL, memory_order_release); /* 奇 -> 偶 */
    h->writer_pid = 0;
    atomic_store_explicit(&h->writer_lock, KEEP_WRITE_LOCK_FREE, memory_order_release);
}

/* ---- Hash 探测：墓碑态（KEEP_TOMBSTONE）不终止探测，消除假性未命中 ---- */
static datarouter_keep_slot *slot_find(
    datarouter_keep_header *h,
    datarouter_keep_slot *slots,
    const uint8_t *arena,
    const void *key, size_t key_len,
    uint64_t hash)
{
    uint32_t cap = h->slot_capacity;
    uint32_t i = (uint32_t)(hash % cap);
    for (uint32_t probe = 0U; probe < cap; probe++) {
        datarouter_keep_slot *s = &slots[i];
        if (s->key_offset == KEEP_EMPTY) {
            return NULL;
        }
        if (s->key_offset != KEEP_TOMBSTONE && s->key_hash == hash) {
            const datarouter_keep_entry_hdr *eh =
                (const datarouter_keep_entry_hdr *)(arena + s->key_offset);
            if (eh->key_len == key_len &&
                memcmp(arena + s->key_offset + sizeof(datarouter_keep_entry_hdr), key, key_len) == 0) {
                return s;
            }
        }
        i = (i + 1u) % cap;
    }
    return NULL;
}

/* 按 arena 偏移定位槽位（淘汰/删除时使用）。 */
static datarouter_keep_slot *slot_find_off(
    datarouter_keep_header *h,
    datarouter_keep_slot *slots,
    uint64_t hash,
    uint32_t off)
{
    uint32_t cap = h->slot_capacity;
    uint32_t i = (uint32_t)(hash % cap);
    for (uint32_t probe = 0U; probe < cap; probe++) {
        datarouter_keep_slot *s = &slots[i];
        if (s->key_offset == KEEP_EMPTY) {
            return NULL;
        }
        if (s->key_offset == off) {
            return s;
        }
        i = (i + 1u) % cap;
    }
    return NULL;
}

static void slot_mark_tombstone(datarouter_keep_header *h, datarouter_keep_slot *s) {
    if (s->key_offset != KEEP_TOMBSTONE && s->key_offset != KEEP_EMPTY) {
        h->tombstones++;
    }
    s->key_offset = KEEP_TOMBSTONE;
}

static int slot_live(const datarouter_keep_slot *s) {
    return s->key_offset != KEEP_EMPTY && s->key_offset != KEEP_TOMBSTONE;
}

/* 原位压实：在写锁 + seqlock（generation 奇）下重建哈希表，清除全部墓碑。
 * 存活槽位引用 arena 偏移仍有效；仅槽位位置变化，读方经两段校验重试。 */
static void keep_rehash(datarouter_keep_header *h) {
    uint32_t cap = h->slot_capacity;
    datarouter_keep_slot *slots = keep_slots(h);
    datarouter_keep_slot *tmp = (datarouter_keep_slot *)malloc((size_t)cap * sizeof(datarouter_keep_slot));
    if (!tmp) {
        return;
    }
    uint32_t n = 0u;
    for (uint32_t i = 0u; i < cap; i++) {
        if (slot_live(&slots[i])) {
            tmp[n++] = slots[i];
        }
    }
    for (uint32_t i = 0u; i < cap; i++) {
        slots[i].key_offset = KEEP_EMPTY;
    }
    for (uint32_t i = 0u; i < n; i++) {
        datarouter_keep_slot *s = &tmp[i];
        uint32_t pos = (uint32_t)(s->key_hash % cap);
        while (slots[pos].key_offset != KEEP_EMPTY) {
            pos = (pos + 1u) % cap;
        }
        slots[pos] = *s;
    }
    free(tmp);
    h->tombstones = 0u;
}

/* 均匀地每隔 N 次写入检查一次墓碑密度；超过阈值即压实。 */
static void keep_compact_if_needed(datarouter_keep_header *h) {
    if (h->tombstones > (h->slot_capacity / 4u)) {
        keep_rehash(h);
    }
}

static int slot_insert(
    datarouter_keep_header *h,
    datarouter_keep_slot *slots,
    uint64_t hash, uint64_t expires,
    uint32_t key_off, uint32_t val_off, uint32_t val_len,
    uint32_t age)
{
    uint32_t cap = h->slot_capacity;
    uint32_t i = (uint32_t)(hash % cap);
    for (uint32_t probe = 0U; probe < cap; probe++) {
        datarouter_keep_slot *s = &slots[i];
        if (s->key_offset == KEEP_EMPTY || s->key_offset == KEEP_TOMBSTONE) {
            if (s->key_offset == KEEP_TOMBSTONE && h->tombstones > 0u) {
                h->tombstones--;
            }
            s->key_hash = hash;
            s->expires_ns = expires;
            s->key_offset = key_off;
            s->value_offset = val_off;
            s->value_len = val_len;
            s->age = age;
            return DATAROUTER_OK;
        }
        i = (i + 1u) % cap;
    }
    return DATAROUTER_ERR_FULL; /* 表已满：防御 */
}

/* ---- 环形 arena：分配前按需淘汰头部条目，保证每条目物理连续（零拷贝 get_ref 前提） ---- */
static void keep_blank_entry_slot(datarouter_keep_header *h, const uint8_t *arena, uint32_t off) {
    const datarouter_keep_entry_hdr *eh = (const datarouter_keep_entry_hdr *)(arena + off);
    uint64_t hash = keep_hash(arena + off + sizeof(datarouter_keep_entry_hdr), eh->key_len);
    datarouter_keep_slot *slots = keep_slots(h);
    datarouter_keep_slot *s = slot_find_off(h, slots, hash, off);
    if (s && s->key_offset == off) {
        slot_mark_tombstone(h, s);
        if (h->entry_count > 0u) {
            h->entry_count--;
        }
    }
}

/* 淘汰最旧条目（逻辑段头部 = evict_head），返回被回收的字节数。
 * write/evict 均为单调逻辑游标（物理位置 = 逻辑 % capacity），故淘汰总能落在活条目上。 */
static uint32_t keep_evict_oldest(datarouter_keep_header *h, uint8_t *arena, uint64_t now) {
    uint32_t cap = h->capacity;
    uint32_t e = (uint32_t)(h->evict_head % cap);
    const datarouter_keep_entry_hdr *eh = (const datarouter_keep_entry_hdr *)(arena + e);
    uint32_t elen = KEEP_ALIGN4(sizeof(datarouter_keep_entry_hdr) + eh->key_len + eh->value_len);
    if (eh->key_len == 0u || elen > cap) {
        /* 逻辑空隙（物理对齐/wrap 的 pad 段），无内部条目：跳过到下一个 capacity 边界。 */
        uint32_t skip = (cap - (e % cap)) % cap;
        if (skip == 0u) {
            skip = cap;
        }
        h->evict_head += skip;
        return skip;
    }
    int expired_p = eh->expires_ns != 0ULL && eh->expires_ns <= now;
    keep_blank_entry_slot(h, arena, e);
    if (expired_p) {
        h->expired++;
    } else {
        h->evictions++;
    }
    h->evict_head += elen; /* 逻辑递增；物理 = %capacity */
    return elen;
}

static int keep_arena_alloc(
    datarouter_keep_header *h,
    uint8_t *arena,
    uint32_t need,
    uint64_t now,
    uint32_t *out_off)
{
    uint32_t cap = h->capacity;
    uint32_t used = h->used;
    uint32_t evicted = 0u;

    /* 1) 保证逻辑总占用 + need <= cap */
    while (used + need > cap) {
        if (used == 0u || evicted >= DATAROUTER_KEEP_EVICT_PER_PUT) {
            break;
        }
        uint32_t freed = keep_evict_oldest(h, arena, now);
        if (freed == 0u) {
            return DATAROUTER_ERR_INTERNAL;
        }
        used -= freed;
        evicted++;
    }

    /* 2) 物理放置（保持每条目物理连续，get_ref 零拷贝前提）：尾部放不下则对齐到
     *    capacity 边界（填充 = 跳过尾部空闲），从物理 0 起写。pad 计入逻辑 used，
     *    必要时再淘汰以容纳。 */
    uint32_t pad = 0u;
    uint32_t wphys = (uint32_t)(h->write_head % cap);
    if (wphys + need > cap) {
        uint64_t aligned = ((uint64_t)h->write_head / cap + 1ULL) * (uint64_t)cap;
        uint64_t diff = aligned - h->write_head;
        pad = (uint32_t)diff;
        while (used + pad + need > cap) {
            if (used == 0u || evicted >= DATAROUTER_KEEP_EVICT_PER_PUT) {
                return DATAROUTER_ERR_FULL;
            }
            uint32_t freed = keep_evict_oldest(h, arena, now);
            if (freed == 0u) {
                return DATAROUTER_ERR_INTERNAL;
            }
            used -= freed;
            evicted++;
        }
        h->write_head = (uint32_t)aligned; /* write_head % cap == 0 */
        wphys = 0u;
    }
    if (wphys + need > cap) {
        return DATAROUTER_ERR_FULL; /* 防御：对齐后总应放下 */
    }

    *out_off = wphys;
    h->write_head += need;
    h->used = used + pad + need;
    return DATAROUTER_OK;
}

/* ---- 权限：位图 + 进程身份匹配当前注册（用已缓存 start_id，不再每次读 /proc） ---- */
static int keep_write_authorized(datarouter_port *port) {
    if (!port || !port->shm) {
        return DATAROUTER_ERR_INVALID_ARGUMENT;
    }
    uint64_t bm = atomic_load_explicit(
        &port->shm->control.keep_write_allow_bitmap, memory_order_relaxed);
    if ((bm & (1ULL << port->slot_index)) == 0ULL) {
        return DATAROUTER_ERR_PERMISSION;
    }
    datarouter_registry_metadata m;
    if (!datarouter_registry_read_active(
            &port->shm->control.registry[port->slot_index], &m)) {
        return DATAROUTER_ERR_PERMISSION;
    }
    /* pid + process_start_id 双因子（后者在 open 时经 /proc 采集并缓存于 port）：
     * 即使 pid 被复用，start_id（进程启动时间）也不同，足以防跨进程复用槽位。 */
    if (m.pid != keep_current_pid() || port->process_start_id == 0ULL ||
        port->process_start_id != m.process_start_id) {
        return DATAROUTER_ERR_PERMISSION;
    }
    return DATAROUTER_OK;
}

int datarouter_keep_get(
    datarouter_port *port,
    const void *key, size_t key_len,
    void *out, size_t out_cap, size_t *out_len)
{
    if (!port || !out || !out_len || (key == NULL && key_len != 0u)) {
        return DATAROUTER_ERR_INVALID_ARGUMENT;
    }
    datarouter_keep_header *h = datarouter_keep_header_of(port->shm);
    if (!h) {
        return DATAROUTER_ERR_NOT_FOUND;
    }
    uint64_t hash = keep_hash(key, key_len);
    datarouter_keep_slot *slots = keep_slots(h);
    uint8_t *arena = keep_arena(h);
    uint64_t now = datarouter_now_ns();
    for (uint32_t retry = 0U; retry < DATAROUTER_KEEP_SEQLOCK_RETRY; retry++) {
        uint64_t g1 = atomic_load_explicit(&h->generation, memory_order_acquire);
        if ((g1 & 1ULL) != 0ULL) {
            keep_recover_if_dead(h);
            continue;
        }
        atomic_thread_fence(memory_order_acquire);
        datarouter_keep_slot *s =
            (key_len != 0u || key != NULL) ? slot_find(h, slots, arena, key, key_len, hash) : NULL;
        const uint8_t *val_ptr = NULL;
        uint32_t val_len = 0u;
        int expired = 0;
        if (s) {
            expired = s->expires_ns != 0ULL && s->expires_ns <= now;
            if (!expired) {
                val_ptr = arena + s->value_offset;
                val_len = s->value_len;
            }
        }
        atomic_thread_fence(memory_order_acquire);
        uint64_t g2 = atomic_load_explicit(&h->generation, memory_order_acquire);
        if (g2 != g1) {
            continue;
        }
        if (!s) {
            h->misses++;
            return DATAROUTER_ERR_NOT_FOUND;
        }
        if (expired) {
            h->misses++;
            h->expired++;
            return DATAROUTER_ERR_NOT_FOUND;
        }
        if (out_cap < val_len) {
            return DATAROUTER_ERR_FULL;
        }
        memcpy(out, val_ptr, val_len);
        *out_len = val_len;
        s->age++;
        h->hits++;
        return DATAROUTER_OK;
    }
    return DATAROUTER_ERR_AGAIN;
}

int datarouter_keep_get_ref(
    datarouter_port *port,
    const void *key, size_t key_len,
    const void **value_out, size_t *value_len_out,
    uint64_t *seq_out)
{
    if (!port || !value_out || !value_len_out || !seq_out ||
        (key == NULL && key_len != 0u)) {
        return DATAROUTER_ERR_INVALID_ARGUMENT;
    }
    datarouter_keep_header *h = datarouter_keep_header_of(port->shm);
    if (!h) {
        return DATAROUTER_ERR_NOT_FOUND;
    }
    uint64_t hash = keep_hash(key, key_len);
    datarouter_keep_slot *slots = keep_slots(h);
    uint8_t *arena = keep_arena(h);
    uint64_t now = datarouter_now_ns();
    for (uint32_t retry = 0U; retry < DATAROUTER_KEEP_SEQLOCK_RETRY; retry++) {
        uint64_t g1 = atomic_load_explicit(&h->generation, memory_order_acquire);
        if ((g1 & 1ULL) != 0ULL) {
            keep_recover_if_dead(h);
            continue;
        }
        atomic_thread_fence(memory_order_acquire);
        datarouter_keep_slot *s = slot_find(h, slots, arena, key, key_len, hash);
        if (s && !(s->expires_ns != 0ULL && s->expires_ns <= now)) {
            *value_out = (const void *)(arena + s->value_offset);
            *value_len_out = s->value_len;
            *seq_out = g1;
            s->age++;
            h->hits++;
            return DATAROUTER_OK;
        }
        if (!s) {
            h->misses++;
            return DATAROUTER_ERR_NOT_FOUND;
        }
        h->misses++;
        h->expired++;
        return DATAROUTER_ERR_NOT_FOUND;
    }
    return DATAROUTER_ERR_AGAIN;
}

int datarouter_keep_verify_ref(datarouter_port *port, uint64_t seq) {
    datarouter_keep_header *h = datarouter_keep_header_of(port->shm);
    if (!h) {
        return DATAROUTER_ERR_NOT_FOUND;
    }
    uint64_t g2 = atomic_load_explicit(&h->generation, memory_order_acquire);
    if ((g2 & 1ULL) == 0ULL && g2 == seq) {
        return DATAROUTER_OK;
    }
    return DATAROUTER_ERR_AGAIN;
}

int datarouter_keep_put(
    datarouter_port *port,
    const void *key, size_t key_len,
    const void *value, size_t value_len,
    uint64_t ttl_ns)
{
    if (!port || (key == NULL && key_len != 0u) ||
        (value == NULL && value_len != 0u) || key_len > DATAROUTER_KEEP_KEY_MAX) {
        return DATAROUTER_ERR_INVALID_ARGUMENT;
    }
    int auth = keep_write_authorized(port);
    if (auth != DATAROUTER_OK) {
        return auth;
    }
    datarouter_keep_header *h = datarouter_keep_header_of(port->shm);
    if (!h) {
        return DATAROUTER_ERR_NOT_FOUND;
    }
    int rc = keep_writer_acquire(h);
    if (rc != DATAROUTER_OK) {
        return rc;
    }
    keep_compact_if_needed(h);
    uint32_t need = KEEP_ALIGN4(sizeof(datarouter_keep_entry_hdr) + (uint32_t)key_len + (uint32_t)value_len);
    uint64_t now = datarouter_now_ns();
    uint64_t expires = ttl_ns == 0ULL ? 0ULL : (now + ttl_ns);
    uint64_t hash = keep_hash(key, key_len);
    datarouter_keep_slot *slots = keep_slots(h);
    uint8_t *arena = keep_arena(h);
    int result;

    /* 同 key 已存在：先按删除语义置空旧槽（Backward-shift），再写入新人。 */
    datarouter_keep_slot *old = slot_find(h, slots, arena, key, key_len, hash);
    if (old) {
        slot_mark_tombstone(h, old);
        if (h->entry_count > 0u) {
            h->entry_count--;
        }
    }

    uint32_t off = 0u;
    rc = keep_arena_alloc(h, arena, need, now, &off);
    if (rc != DATAROUTER_OK) {
        keep_writer_release(h);
        return rc;
    }
    datarouter_keep_entry_hdr *eh = (datarouter_keep_entry_hdr *)(arena + off);
    eh->key_len = (uint32_t)key_len;
    eh->value_len = (uint32_t)value_len;
    eh->expires_ns = expires;
    if (key_len != 0u) {
        memcpy(arena + off + sizeof(datarouter_keep_entry_hdr), key, key_len);
    }
    if (value_len != 0u) {
        memcpy(arena + off + sizeof(datarouter_keep_entry_hdr) + key_len, value, value_len);
    }
    atomic_thread_fence(memory_order_release); /* 先写 arena 字节，再发布槽位 */
    int ir = slot_insert(h, slots, hash, expires,
                off, off + (uint32_t)sizeof(datarouter_keep_entry_hdr) + (uint32_t)key_len,
                (uint32_t)value_len, 1u);
    if (ir != DATAROUTER_OK) {
        keep_writer_release(h);
        return ir;
    }
    h->entry_count++;
    keep_writer_release(h);
    result = DATAROUTER_OK;
    return result;
}

int datarouter_keep_invalidate(datarouter_port *port, const void *key, size_t key_len) {
    if (!port || (key == NULL && key_len != 0u)) {
        return DATAROUTER_ERR_INVALID_ARGUMENT;
    }
    int auth = keep_write_authorized(port);
    if (auth != DATAROUTER_OK) {
        return auth;
    }
    datarouter_keep_header *h = datarouter_keep_header_of(port->shm);
    if (!h) {
        return DATAROUTER_ERR_NOT_FOUND;
    }
    int rc = keep_writer_acquire(h);
    if (rc != DATAROUTER_OK) {
        return rc;
    }
    uint64_t hash = keep_hash(key, key_len);
    datarouter_keep_slot *slots = keep_slots(h);
    uint8_t *arena = keep_arena(h);
    datarouter_keep_slot *s = slot_find(h, slots, arena, key, key_len, hash);
    if (!s) {
        keep_writer_release(h);
        return DATAROUTER_ERR_NOT_FOUND;
    }
    slot_mark_tombstone(h, s);
    if (h->entry_count > 0u) {
        h->entry_count--;
    }
    keep_writer_release(h);
    return DATAROUTER_OK;
}

int datarouter_keep_invalidate_prefix(datarouter_port *port, const void *prefix, size_t prefix_len) {
    if (!port || (prefix == NULL && prefix_len != 0u)) {
        return DATAROUTER_ERR_INVALID_ARGUMENT;
    }
    int auth = keep_write_authorized(port);
    if (auth != DATAROUTER_OK) {
        return auth;
    }
    datarouter_keep_header *h = datarouter_keep_header_of(port->shm);
    if (!h) {
        return DATAROUTER_ERR_NOT_FOUND;
    }
    int rc = keep_writer_acquire(h);
    if (rc != DATAROUTER_OK) {
        return rc;
    }
    datarouter_keep_slot *slots = keep_slots(h);
    uint8_t *arena = keep_arena(h);
    uint32_t cap = h->slot_capacity;
    uint32_t scanned = 0u;
    uint32_t removed = 0u;
    /* 空槽过滤 + 前缀字节比较；分片限流（即使中途超上限也返回已处理的部分完成语义）。 */
    for (uint32_t i = 0u; i < cap; i++) {
        datarouter_keep_slot *s = &slots[i];
        if (s->key_offset == KEEP_EMPTY || s->key_offset == KEEP_TOMBSTONE ||
            s->key_hash == 0ULL) {
            continue;
        }
        if (++scanned > DATAROUTER_KEEP_PREFIX_SCAN_LIMIT) {
            break;
        }
        const datarouter_keep_entry_hdr *eh = (const datarouter_keep_entry_hdr *)(arena + s->key_offset);
        if (eh->key_len < prefix_len) {
            continue;
        }
        const uint8_t *k = arena + s->key_offset + sizeof(datarouter_keep_entry_hdr);
        if (prefix_len == 0u) {
            /* prefix_len==0 视为全清语义 */
        } else if (memcmp(k, prefix, prefix_len) != 0) {
            continue;
        }
        slot_mark_tombstone(h, s);
        removed++;
        if (h->entry_count > 0u) {
            h->entry_count--;
        }
    }
    keep_writer_release(h);
    (void)arena;
    return scanned >= DATAROUTER_KEEP_PREFIX_SCAN_LIMIT ? DATAROUTER_ERR_AGAIN : DATAROUTER_OK;
}

int datarouter_keep_stats(datarouter_port *port, datarouter_keep_stats_t *out) {
    if (!port || !out) {
        return DATAROUTER_ERR_INVALID_ARGUMENT;
    }
    datarouter_keep_header *h = datarouter_keep_header_of(port->shm);
    if (!h) {
        return DATAROUTER_ERR_NOT_FOUND;
    }
    out->capacity = h->capacity;
    out->entry_count = h->entry_count;
    out->hits = h->hits;
    out->misses = h->misses;
    out->expired = h->expired;
    out->evictions = h->evictions;
    out->generation = atomic_load_explicit(&h->generation, memory_order_relaxed);
    out->recovery_seq = atomic_load_explicit(&h->recovery_seq, memory_order_relaxed);
    out->slot_capacity = h->slot_capacity;
    return DATAROUTER_OK;
}
