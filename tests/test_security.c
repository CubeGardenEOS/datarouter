#define _GNU_SOURCE
#include "datarouter_internal.h"
#include "datarouter_registry.h"
#include "datarouter_protocol.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>

#ifdef DATAROUTER_TEST_WRAP_PID
/* Pause precisely after KEEP has read its authorization snapshot. Linker
 * wrapping affects this test's C objects, not sanitizer shared libraries. */
static _Thread_local int pause_pid;
static atomic_int stopped_pid;
static atomic_int resume_pid;
extern pid_t __real_getpid(void);
pid_t __wrap_getpid(void) {
    if (pause_pid) {
        pause_pid = 0;
        atomic_store(&stopped_pid, 1);
        while (!atomic_load(&resume_pid)) datarouter_yield();
    }
    return __real_getpid();
}
typedef struct { datarouter_port *port; int result; } paused_write;
static void *write_after_pause(void *arg) {
    paused_write *work = arg;
    pause_pid = 1;
    work->result = datarouter_keep_put(work->port, "stale", 5, "value", 5, 0);
    return NULL;
}
#endif
#include <sys/mman.h>
#include <sys/wait.h>
#include <signal.h>
#if DATAROUTER_LAYOUT_VERSION >= 14
#define WRITER_GATE rx_writer_owner
#else
#define WRITER_GATE direct_writers
#endif

static void keep_boundaries(void) {
    size_t size = datarouter_shared_memory_size_with_keep(2, 8192);
    datarouter_shared_memory *shm = calloc(1, size);
    assert(shm);
    assert(datarouter_init_shared_memory_with_keep(shm, 2, 60000, 0, 8192, UINT32_MAX)
           == DATAROUTER_ERR_INVALID_ARGUMENT);
    assert(datarouter_init_shared_memory_with_keep(shm, 2, 60000, 0, 8192, 32) == DATAROUTER_OK);
    assert(datarouter_validate_shared_memory(shm, size) == DATAROUTER_OK);
    datarouter_keep_header *h = datarouter_keep_header_of(shm);
    uint32_t cap = h->slot_capacity;
    h->slot_capacity = UINT32_MAX;
    assert(datarouter_validate_shared_memory(shm, size) == DATAROUTER_ERR_PROTOCOL);
    h->slot_capacity = cap;
    datarouter_port port = {0};
    port.shm = shm;
    port.map_size = size;
    port.slot_index = 64;
    assert(datarouter_keep_put(&port, NULL, 0, NULL, 0, 0) == DATAROUTER_ERR_INVALID_ARGUMENT);
    free(shm);
}

static void keep_operations(const char *scenario) {
    size_t size = datarouter_shared_memory_size_with_keep(2, 1024*1024);
    datarouter_shared_memory *shm = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    assert(shm != MAP_FAILED);
    assert(datarouter_init_shared_memory_with_keep(shm, 2, 60000, 0, 1024*1024, 8192) == DATAROUTER_OK);
    datarouter_port p = {0};
    p.shm = shm; p.map_size = size; p.slot_index = 0; p.route_id = 64;
    memcpy(p.service_name, "core", 5);
    uint32_t uid;
    assert(datarouter_process_identity(getpid(), &uid, &p.process_start_id) == DATAROUTER_OK);
    datarouter_registry_entry *e = &shm->control.registry[0];
    atomic_store(&e->pid, getpid()); atomic_store(&e->uid, uid);
    atomic_store(&e->process_start_id, p.process_start_id);
    atomic_store(&e->route_id, p.route_id); atomic_store(&e->state, DATAROUTER_PORT_ACTIVE);
#if DATAROUTER_LAYOUT_VERSION >= 15
    atomic_store(&e->io_users, (unsigned long long)p.route_id << 32U);
#endif
    e->generation = 1;
    datarouter_registry_store_name(e, "core");
    atomic_store(&shm->control.keep_write_allow_bitmap, 1);
    char out[2048]; size_t len = 0;
    if (!strcmp(scenario, "keep_write_lifetime") || !strcmp(scenario, "keep_write_revoked")) {
#ifdef DATAROUTER_TEST_WRAP_PID
        paused_write work = { .port = &p, .result = -1 };
        pthread_t thread;
        assert(pthread_create(&thread, NULL, write_after_pause, &work) == 0);
        uint64_t deadline = datarouter_now_ns() + 2000000000ULL;
        while (!atomic_load(&stopped_pid)) {
            assert(datarouter_now_ns() < deadline);
            datarouter_yield();
        }
        if (!strcmp(scenario, "keep_write_revoked"))
            atomic_store(&shm->control.keep_write_allow_bitmap, 0);
        else
            assert(datarouter_registry_release_slot(shm, 0) == 1);
        atomic_store(&resume_pid, 1);
        assert(pthread_join(thread, NULL) == 0);
        assert(work.result == DATAROUTER_ERR_PERMISSION);
        assert(datarouter_keep_get(&p, "stale", 5, out, sizeof(out), &len) == DATAROUTER_ERR_NOT_FOUND);
#else
        assert(!"this test requires --wrap=getpid");
#endif
    } else if (!strcmp(scenario, "key_limit")) {
        char key[DATAROUTER_KEEP_KEY_MAX + 1U] = {0};
        assert(datarouter_keep_invalidate(&p, key, sizeof(key)) == DATAROUTER_ERR_INVALID_ARGUMENT);
    } else if (!strcmp(scenario, "empty")) {
        assert(datarouter_keep_put(&p, NULL, 0, NULL, 0, 0) == DATAROUTER_OK);
        assert(datarouter_keep_get(&p, NULL, 0, NULL, 0, &len) == DATAROUTER_OK && len == 0);
        assert(datarouter_keep_put(&p, "k", 1, "value", 5, 0) == DATAROUTER_OK);
        assert(datarouter_keep_get(&p, "k", 1, out, 1, &len) == DATAROUTER_ERR_FULL && len == 5);
        assert(datarouter_keep_get(&p, "k", 1, out, sizeof(out), &len) == DATAROUTER_OK);
        assert(len == 5 && memcmp(out, "value", 5) == 0);
    } else if (!strcmp(scenario, "ttl")) {
        assert(datarouter_keep_put(&p, "k", 1, "v", 1, UINT64_MAX) == DATAROUTER_ERR_INVALID_ARGUMENT);
    } else if (!strcmp(scenario, "lease")) {
        assert(datarouter_keep_put(&p, "k", 1, "before", 6, 0) == DATAROUTER_OK);
        const void *ref; uint64_t seq;
        assert(datarouter_keep_get_ref(&p, "k", 1, &ref, &len, &seq) == DATAROUTER_OK);
        assert(datarouter_keep_put(&p, "k", 1, "after", 5, 0) == DATAROUTER_ERR_AGAIN);
        assert(len == 6 && memcmp(ref, "before", 6) == 0);
        assert(datarouter_keep_verify_ref(&p, seq+2) == DATAROUTER_ERR_AGAIN);
        assert(datarouter_keep_verify_ref(&p, seq) == DATAROUTER_OK);
        assert(datarouter_keep_verify_ref(&p, seq) == DATAROUTER_ERR_AGAIN);
        assert(datarouter_keep_put(&p, "k", 1, "after", 5, 0) == DATAROUTER_OK);
    } else if (!strcmp(scenario, "wrap")) {
        for (unsigned i = 0; i < 4000; ++i) {
            size_t n = 1 + (i * 13) % sizeof(out);
            memset(out, i % 255, n);
            assert(datarouter_keep_put(&p, "k", 1, out, n, 0) == DATAROUTER_OK);
            memset(out, 0, sizeof(out));
            assert(datarouter_keep_get(&p, "k", 1, out, sizeof(out), &len) == DATAROUTER_OK);
            assert(len == n);
            for (size_t j = 0; j < n; ++j) assert((unsigned char)out[j] == i % 255);
        }
    } else if (!strcmp(scenario, "prefix")) {
        for (unsigned i = 0; i < 6000; ++i) {
            char key[24]; int n = snprintf(key, sizeof(key), "key-%u", i);
            assert(datarouter_keep_put(&p, key, n, "v", 1, 0) == DATAROUTER_OK);
        }
        assert(datarouter_keep_invalidate_prefix(&p, "absent", 6) == DATAROUTER_OK);
        assert(datarouter_keep_invalidate_prefix(&p, NULL, 0) == DATAROUTER_OK);
        datarouter_keep_stats_t stats;
        assert(datarouter_keep_stats(&p, &stats) == DATAROUTER_OK && stats.entry_count == 0);
    } else if (!strcmp(scenario, "recovery")) {
        assert(datarouter_keep_put(&p, "k", 1, "before", 6, 0) == DATAROUTER_OK);
        int ready[2]; assert(pipe(ready) == 0);
        pid_t child = fork(); assert(child >= 0);
        if (child == 0) {
            close(ready[0]);
            const void *ref; size_t length; uint64_t seq;
            if (datarouter_keep_get_ref(&p, "k", 1, &ref, &length, &seq) != DATAROUTER_OK) _exit(2);
            /* Emulate a partial mutation while holding ownership. Recovery
             * must discard all cache state rather than publishing it. */
            datarouter_keep_header_of(shm)->used = UINT32_MAX;
            if (write(ready[1], "x", 1) != 1) _exit(3);
            for (;;) pause();
        }
        close(ready[1]); char byte;
        assert(read(ready[0], &byte, 1) == 1); close(ready[0]);
        assert(datarouter_keep_get(&p, "k", 1, out, sizeof(out), &len) == DATAROUTER_ERR_AGAIN);
        assert(kill(child, SIGKILL) == 0);
        int status; assert(waitpid(child, &status, 0) == child && WIFSIGNALED(status));
        assert(datarouter_keep_get(&p, "k", 1, out, sizeof(out), &len) == DATAROUTER_ERR_NOT_FOUND);
        datarouter_keep_stats_t stats;
        assert(datarouter_keep_stats(&p, &stats) == DATAROUTER_OK);
        assert(stats.recovery_seq == 1 && stats.entry_count == 0);
        assert(datarouter_keep_put(&p, "k", 1, "after", 5, 0) == DATAROUTER_OK);
    } else if (!strcmp(scenario, "pin_lifetime")) {
#if DATAROUTER_LAYOUT_VERSION >= 15
        unsigned long long stale = atomic_load(&e->io_users);
#else
        unsigned int stale = atomic_load(&e->io_users);
#endif
        assert(datarouter_registry_pin_self(e, getpid(), uid, p.process_start_id, p.route_id, "core"));
        assert(datarouter_registry_release_slot(shm, 0) == 0);
        assert(!datarouter_registry_pin_self(e, getpid(), uid, p.process_start_id, p.route_id, "core"));
        datarouter_registry_unpin(e);
        datarouter_registry_cleanup_releasing(shm);
        assert(atomic_load(&e->state) == DATAROUTER_PORT_EMPTY);
        /* A claimant paused with the old open counter must not acquire a
         * reference after the closer has retired that registration. */
        assert(!atomic_compare_exchange_strong(&e->io_users, &stale, stale + 1));
        assert(datarouter_registry_empty_slot_ready(e));
    } else if (!strcmp(scenario, "writer_lifetime")) {
        atomic_store(&e->WRITER_GATE, (unsigned int)getpid());
        assert(datarouter_registry_release_slot(shm, 0) == 0);
        assert(atomic_load(&e->WRITER_GATE) == (unsigned int)getpid());
        atomic_store(&e->WRITER_GATE, 0);
        datarouter_registry_cleanup_releasing(shm);
        assert(atomic_load(&e->state) == DATAROUTER_PORT_EMPTY);
        /* Clearing metadata must never reset the separate lifetime gate. */
        atomic_store(&e->WRITER_GATE, (unsigned int)getpid());
        datarouter_registry_clear_entry(e, 0);
        assert(atomic_load(&e->WRITER_GATE) == (unsigned int)getpid());
        atomic_store(&e->WRITER_GATE, 0);
    } else if (!strcmp(scenario, "generation")) {
        atomic_store(&e->route_id, 128);
        assert(datarouter_keep_put(&p, "k", 1, "v", 1, 0) == DATAROUTER_ERR_PERMISSION);
    } else { assert(0); }
    assert(munmap(shm, size) == 0);
}

int main(int argc, char **argv) {
    assert(argc == 2);
    datarouter_frame_layout layout;
    datarouter_chunk_meta chunk;
    if (!strcmp(argv[1], "tlvs")) {
        assert(datarouter_protocol_validate_tlvs(NULL, 3) == 0);
        assert(datarouter_protocol_validate_tlvs(NULL, 0) == 1);
    } else if (!strcmp(argv[1], "layout")) {
        assert(datarouter_protocol_frame_layout(NULL, 8, &layout) == DATAROUTER_ERR_INVALID_ARGUMENT);
    } else if (!strcmp(argv[1], "chunk")) {
        assert(datarouter_protocol_chunk_metadata(NULL, 3, &chunk) == DATAROUTER_ERR_INVALID_ARGUMENT);
    } else if (!strcmp(argv[1], "ref")) {
        assert(datarouter_keep_verify_ref(NULL, 0) == DATAROUTER_ERR_INVALID_ARGUMENT);
    } else if (!strcmp(argv[1], "keep")) {
        keep_boundaries();
    } else { keep_operations(argv[1]); }
    puts("security case passed");
}
