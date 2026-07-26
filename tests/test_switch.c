#include "../src/datarouter_internal.h"
#include "../src/datarouter_registry.h"
#include "../src/datarouter_router.h"
#include "../src/datarouter_scheduler.h"

#include <errno.h>
#include <fcntl.h>
#ifndef _WIN32
#include <pthread.h>
#endif
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

static void require_int(int ok, const char *message) {
    if (!ok) {
        fprintf(stderr, "test_switch failed: %s\n", message);
        exit(1);
    }
}

static void unlink_shm(const char *name) {
    shm_unlink(name);
}

static pid_t start_switch(const char *name, uint32_t ttl_ms) {
    pid_t pid = fork();
    require_int(pid >= 0, "fork switch");
    if (pid == 0) {
        char ttl[32];
        snprintf(ttl, sizeof(ttl), "%u", ttl_ms);
        execl(
            "./build/datarouter-switch",
            "datarouter-switch",
            "--name",
            name,
            "--ttl-ms",
            ttl,
            "--allow",
            "Core,MLHelper,ERP,CMS",
            (char *)NULL
        );
        _exit(127);
    }
    datarouter_sleep_us(50000U);
    return pid;
}

static pid_t start_switch_reaping(const char *name, uint32_t ttl_ms) {
    pid_t pid = fork();
    require_int(pid >= 0, "fork reaping switch");
    if (pid == 0) {
        char ttl[32];
        snprintf(ttl, sizeof(ttl), "%u", ttl_ms);
        execl(
            "./build/datarouter-switch",
            "datarouter-switch",
            "--name",
            name,
            "--ttl-ms",
            ttl,
            "--allow",
            "Core,MLHelper,ERP,CMS",
            "--reap-dead-processes",
            (char *)NULL
        );
        _exit(127);
    }
    datarouter_sleep_us(50000U);
    return pid;
}

static void write_config(const char *path, const char *allow_port) {
    FILE *file = fopen(path, "w");
    require_int(file != NULL, "open switch config");
    fprintf(file, "[common]\nallow_port=%s\nsocket_raw=1\nsocket_kcp=0\n", allow_port);
    fclose(file);
}

static pid_t start_switch_config(const char *name, uint32_t ttl_ms, const char *config_path) {
    pid_t pid = fork();
    require_int(pid >= 0, "fork switch config");
    if (pid == 0) {
        char ttl[32];
        snprintf(ttl, sizeof(ttl), "%u", ttl_ms);
        execl(
            "./build/datarouter-switch",
            "datarouter-switch",
            "--name",
            name,
            "--ttl-ms",
            ttl,
            "--config",
            config_path,
            (char *)NULL
        );
        _exit(127);
    }
    datarouter_sleep_us(50000U);
    return pid;
}

static void stop_switch(pid_t pid) {
    kill(pid, SIGTERM);
    waitpid(pid, NULL, 0);
}

static void logon(datarouter_port *port, const char *service) {
    char err[128];
    int rc = datarouter_logon(port, service, err, sizeof(err));
    if (rc != DATAROUTER_OK) {
        fprintf(stderr, "logon %s failed: %s (%s)\n", service, datarouter_strerror(rc), err);
        exit(1);
    }
}

static pid_t start_logged_service(const char *name, const char *service, int ready_fd) {
    pid_t pid = fork();
    require_int(pid >= 0, "fork logged service");
    if (pid == 0) {
        datarouter_options opts;
        opts.shm_name = name;
        opts.timeout_ms = 1000U;

        datarouter_port *port = NULL;
        if (datarouter_open(&opts, &port) != DATAROUTER_OK) {
            _exit(2);
        }
        char err[128];
        if (datarouter_logon(port, service, err, sizeof(err)) != DATAROUTER_OK) {
            _exit(3);
        }
        const char ok = '1';
        if (write(ready_fd, &ok, 1U) != 1) {
            _exit(4);
        }
        for (;;) {
            pause();
        }
    }
    return pid;
}

static pid_t start_logged_service_wait(const char *name, const char *service) {
    int fds[2];
    require_int(pipe(fds) == 0, "pipe logged service");
    pid_t pid = start_logged_service(name, service, fds[1]);
    close(fds[1]);
    char ok = 0;
    require_int(read(fds[0], &ok, 1U) == 1 && ok == '1', "logged service ready");
    close(fds[0]);
    return pid;
}

static void stop_logged_service(pid_t pid) {
    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
}

static void activate_test_entry_pid(datarouter_shared_memory *shm, uint32_t slot, const char *service, pid_t pid) {
    datarouter_registry_entry *entry = &shm->control.registry[slot];
    uint32_t generation = (entry->generation + 1U) & DATAROUTER_ROUTE_GENERATION_MASK;
    if (generation == 0U) {
        generation = 1U;
    }
    entry->slot_index = slot;
    entry->generation = generation;
    entry->route_id = datarouter_route_for_slot_generation(slot, generation);
    entry->pid = pid;
    entry->uid = getuid();
    uint32_t identity_uid = 0U;
    uint64_t process_start_id = 1ULL;
    if (datarouter_process_identity(pid, &identity_uid, &process_start_id) == DATAROUTER_OK) {
        entry->uid = identity_uid;
    }
    entry->process_start_id = process_start_id;
    datarouter_registry_store_name(entry, service);
    atomic_store_explicit(&entry->last_active_ns, datarouter_now_ns(), memory_order_release);
    atomic_store_explicit(&entry->state, DATAROUTER_PORT_ACTIVE, memory_order_release);
}

static void activate_test_entry(datarouter_shared_memory *shm, uint32_t slot, const char *service) {
    activate_test_entry_pid(shm, slot, service, getpid());
}

static void init_test_port(
    datarouter_port *port,
    datarouter_shared_memory *shm,
    uint32_t slot,
    const char *service
) {
    memset(port, 0, sizeof(*port));
    port->fd = -1;
    port->shm = shm;
    port->slot_index = slot;
    port->route_id = atomic_load_explicit(
        &shm->control.registry[slot].route_id,
        memory_order_acquire
    );
    port->process_start_id = atomic_load_explicit(
        &shm->control.registry[slot].process_start_id,
        memory_order_acquire
    );
    port->switch_pid = shm->control.switch_pid;
    port->switch_uid = shm->control.switch_uid;
    port->switch_start_id = shm->control.switch_start_id;
    atomic_init(&port->last_switch_identity_check_ns, datarouter_now_ns());
    atomic_init(&port->switch_dead, 0U);
    snprintf(port->service_name, sizeof(port->service_name), "%s", service);
}

static void encode_test_header(uint8_t header[DATAROUTER_FRAME_HEADER_SIZE], uint32_t route, uint32_t len) {
    header[0] = (uint8_t)(route & 0xffU);
    header[1] = (uint8_t)((route >> 8U) & 0xffU);
    header[2] = (uint8_t)((route >> 16U) & 0xffU);
    header[3] = (uint8_t)((route >> 24U) & 0xffU);
    header[4] = (uint8_t)(len & 0xffU);
    header[5] = (uint8_t)((len >> 8U) & 0xffU);
    header[6] = (uint8_t)((len >> 16U) & 0xffU);
    header[7] = (uint8_t)((len >> 24U) & 0xffU);
}

static void write_test_frame(datarouter_shared_memory *shm, uint32_t source_slot, uint32_t target_route, const char *payload) {
    uint32_t len = (uint32_t)strlen(payload) + 1U;
    uint8_t header[DATAROUTER_FRAME_HEADER_SIZE];
    encode_test_header(header, target_route, len);
    datarouter_channel *tx = datarouter_tx_channel(shm, source_slot);
    require_int(datarouter_ring_write(tx, header, sizeof(header)) == DATAROUTER_OK, "write test frame header");
    require_int(datarouter_ring_write(tx, (const uint8_t *)payload, len) == DATAROUTER_OK, "write test frame payload");
}

typedef struct {
    size_t count;
    char sources[8][DATAROUTER_SERVICE_NAME_MAX + 1U];
    char payloads[8][32];
} drain_records;

#ifndef _WIN32
#define CONCURRENT_SEND_THREADS 4U
#define CONCURRENT_SENDS_PER_THREAD 256U
#define CONCURRENT_SEND_TOTAL (CONCURRENT_SEND_THREADS * CONCURRENT_SENDS_PER_THREAD)

typedef struct {
    datarouter_port *sender;
    unsigned int thread_id;
    int rc;
} concurrent_sender_args;

typedef struct {
    datarouter_port *receiver;
    unsigned char seen[CONCURRENT_SEND_THREADS][CONCURRENT_SENDS_PER_THREAD];
    size_t received;
    int rc;
    atomic_int *senders_done;
} concurrent_receiver_args;
#endif

static int record_drain_callback(
    const datarouter_message_info *info,
    const void *data,
    size_t len,
    void *user_data
) {
    drain_records *records = (drain_records *)user_data;
    require_int(records->count < 8U, "drain callback capacity");
    snprintf(records->sources[records->count], sizeof(records->sources[records->count]), "%s", info->source);
    size_t copy = len >= sizeof(records->payloads[records->count])
        ? sizeof(records->payloads[records->count]) - 1U
        : len;
    memcpy(records->payloads[records->count], data, copy);
    records->payloads[records->count][copy] = '\0';
    records->count++;
    return DATAROUTER_OK;
}

#ifndef _WIN32
static void *concurrent_sender_main(void *arg) {
    concurrent_sender_args *sender = (concurrent_sender_args *)arg;
    sender->rc = DATAROUTER_OK;

    for (unsigned int seq = 0U; seq < CONCURRENT_SENDS_PER_THREAD; seq++) {
        char payload[32];
        int written = snprintf(payload, sizeof(payload), "t=%u seq=%u", sender->thread_id, seq);
        if (written < 0 || (size_t)written >= sizeof(payload)) {
            sender->rc = DATAROUTER_ERR_INTERNAL;
            return NULL;
        }

        uint64_t started = datarouter_now_ns();
        for (;;) {
            int rc = datarouter_send(sender->sender, "Core", payload, (size_t)written + 1U);
            if (rc == DATAROUTER_OK) {
                break;
            }
            if (rc != DATAROUTER_ERR_FULL) {
                sender->rc = rc;
                return NULL;
            }
            if (datarouter_now_ns() - started > 5000000000ULL) {
                sender->rc = rc;
                return NULL;
            }
            datarouter_sleep_us(500U);
        }
    }

    return NULL;
}

static void *concurrent_receiver_main(void *arg) {
    concurrent_receiver_args *receiver = (concurrent_receiver_args *)arg;
    receiver->rc = DATAROUTER_OK;
    uint64_t started = datarouter_now_ns();

    while (receiver->received < CONCURRENT_SEND_TOTAL) {
        if (datarouter_now_ns() - started > 10000000000ULL) {
            receiver->rc = DATAROUTER_ERR_TIMEOUT;
            return NULL;
        }

        int wait_rc = datarouter_wait(receiver->receiver, 50U);
        if (wait_rc != DATAROUTER_OK && wait_rc != DATAROUTER_ERR_TIMEOUT) {
            receiver->rc = wait_rc;
            return NULL;
        }
        if (wait_rc == DATAROUTER_ERR_TIMEOUT && atomic_load_explicit(receiver->senders_done, memory_order_acquire)) {
            continue;
        }

        for (;;) {
            char payload[64];
            datarouter_message_info info;
            int rc = datarouter_recv(receiver->receiver, payload, sizeof(payload), &info);
            if (rc == DATAROUTER_ERR_AGAIN) {
                break;
            }
            if (rc != DATAROUTER_OK) {
                receiver->rc = rc;
                return NULL;
            }
            require_int(strcmp(info.source, "MLHelper") == 0, "concurrent source service");

            unsigned int thread_id = 0U;
            unsigned int seq = 0U;
            require_int(sscanf(payload, "t=%u seq=%u", &thread_id, &seq) == 2, "concurrent payload format");
            require_int(thread_id < CONCURRENT_SEND_THREADS, "concurrent payload thread id");
            require_int(seq < CONCURRENT_SENDS_PER_THREAD, "concurrent payload sequence");
            require_int(receiver->seen[thread_id][seq] == 0U, "concurrent duplicate payload");
            receiver->seen[thread_id][seq] = 1U;
            receiver->received++;
            started = datarouter_now_ns();
            if (receiver->received == CONCURRENT_SEND_TOTAL) {
                break;
            }
        }
    }

    return NULL;
}
#endif

static void test_route_and_allowlist(void) {
    const char *name = "/datarouter-test";
    unlink_shm(name);
    pid_t sw = start_switch(name, 5000U);

    datarouter_options opts;
    opts.shm_name = name;
    opts.timeout_ms = 1000U;

    datarouter_port *core = NULL;
    datarouter_port *ml = NULL;
    require_int(datarouter_open(&opts, &core) == DATAROUTER_OK, "open core");
    require_int(datarouter_open(&opts, &ml) == DATAROUTER_OK, "open ml");
    logon(core, "Core");
    logon(ml, "MLHelper");

    datarouter_registry_snapshot_entry snapshot[4];
    size_t snapshot_count = 0U;
    require_int(
        datarouter_registry_snapshot(core, snapshot, 4U, &snapshot_count) == DATAROUTER_OK,
        "registry snapshot succeeds"
    );
    require_int(snapshot_count == 2U, "registry snapshot count");
    require_int(strcmp(snapshot[0].service_name, "Core") == 0, "registry snapshot service");
    require_int(snapshot[0].pid > 0, "registry snapshot pid");
    require_int(snapshot[0].route_id > 0U, "registry snapshot route");
    require_int(snapshot[0].channel_id == 0U, "registry snapshot channel");

    const char payload[] = "[{\"type\":\"ping\",\"value\":42}]";
    require_int(
        datarouter_send(ml, "Core", payload, sizeof(payload)) == DATAROUTER_OK,
        "send MLHelper to Core"
    );

    char received[128];
    datarouter_message_info info;
    int rc = DATAROUTER_ERR_AGAIN;
    rc = datarouter_wait(core, 1000U);
    require_int(rc == DATAROUTER_OK, "Core wait sees frame");
    for (int i = 0; i < 200; i++) {
        rc = datarouter_recv(core, received, sizeof(received), &info);
        if (rc == DATAROUTER_OK) {
            break;
        }
        datarouter_sleep_us(10000U);
    }
    require_int(rc == DATAROUTER_OK, "Core receives frame");
    require_int(info.len == sizeof(payload), "payload len");
    require_int(strcmp(info.source, "MLHelper") == 0, "source service");
    require_int(memcmp(received, payload, sizeof(payload)) == 0, "payload bytes");

    datarouter_port *erp = NULL;
    datarouter_port *cms = NULL;
    datarouter_port *extra = NULL;
    require_int(datarouter_open(&opts, &erp) == DATAROUTER_OK, "open erp");
    require_int(datarouter_open(&opts, &cms) == DATAROUTER_OK, "open cms");
    require_int(datarouter_open(&opts, &extra) == DATAROUTER_OK, "open extra");
    logon(erp, "ERP");
    logon(cms, "CMS");
    char err[128];
    require_int(
        datarouter_logon(extra, "Extra", err, sizeof(err)) == DATAROUTER_ERR_PERMISSION,
        "allowlist rejects extra service"
    );

    datarouter_close(extra);
    datarouter_close(cms);
    datarouter_close(erp);
    datarouter_close(ml);
    datarouter_close(core);
    stop_switch(sw);
    unlink_shm(name);
}

#ifndef _WIN32
static void test_concurrent_send_from_shared_port(void) {
    const char *name = "/datarouter-concurrent-send-test";
    unlink_shm(name);
    pid_t sw = start_switch(name, 5000U);

    datarouter_options opts;
    opts.shm_name = name;
    opts.timeout_ms = 1000U;

    datarouter_port *core = NULL;
    datarouter_port *ml = NULL;
    require_int(datarouter_open(&opts, &core) == DATAROUTER_OK, "open concurrent core");
    require_int(datarouter_open(&opts, &ml) == DATAROUTER_OK, "open concurrent ml");
    logon(core, "Core");
    logon(ml, "MLHelper");

    atomic_int senders_done;
    atomic_init(&senders_done, 0);
    concurrent_receiver_args receiver_args;
    memset(&receiver_args, 0, sizeof(receiver_args));
    receiver_args.receiver = core;
    receiver_args.senders_done = &senders_done;

    pthread_t receiver_thread;
    require_int(
        pthread_create(&receiver_thread, NULL, concurrent_receiver_main, &receiver_args) == 0,
        "create concurrent receiver"
    );

    pthread_t sender_threads[CONCURRENT_SEND_THREADS];
    concurrent_sender_args sender_args[CONCURRENT_SEND_THREADS];
    memset(sender_args, 0, sizeof(sender_args));
    for (unsigned int i = 0U; i < CONCURRENT_SEND_THREADS; i++) {
        sender_args[i].sender = ml;
        sender_args[i].thread_id = i;
        require_int(
            pthread_create(&sender_threads[i], NULL, concurrent_sender_main, &sender_args[i]) == 0,
            "create concurrent sender"
        );
    }

    for (unsigned int i = 0U; i < CONCURRENT_SEND_THREADS; i++) {
        require_int(pthread_join(sender_threads[i], NULL) == 0, "join concurrent sender");
        require_int(sender_args[i].rc == DATAROUTER_OK, "concurrent sender succeeds");
    }
    atomic_store_explicit(&senders_done, 1, memory_order_release);

    require_int(pthread_join(receiver_thread, NULL) == 0, "join concurrent receiver");
    require_int(receiver_args.rc == DATAROUTER_OK, "concurrent receiver succeeds");
    require_int(receiver_args.received == CONCURRENT_SEND_TOTAL, "concurrent receiver count");

    for (unsigned int thread_id = 0U; thread_id < CONCURRENT_SEND_THREADS; thread_id++) {
        for (unsigned int seq = 0U; seq < CONCURRENT_SENDS_PER_THREAD; seq++) {
            require_int(receiver_args.seen[thread_id][seq] == 1U, "concurrent payload delivered");
        }
    }

    datarouter_close(ml);
    datarouter_close(core);
    stop_switch(sw);
    unlink_shm(name);
}
#endif

static void test_wait_timeout(void) {
    const char *name = "/datarouter-wait-test";
    unlink_shm(name);
    pid_t sw = start_switch(name, 5000U);

    datarouter_options opts;
    opts.shm_name = name;
    opts.timeout_ms = 1000U;

    datarouter_port *core = NULL;
    require_int(datarouter_open(&opts, &core) == DATAROUTER_OK, "open core wait");
    logon(core, "Core");
    require_int(datarouter_wait(core, 10U) == DATAROUTER_ERR_TIMEOUT, "wait times out without frames");

    datarouter_close(core);
    stop_switch(sw);
    unlink_shm(name);
}

static void test_active_service_not_replaced_before_ttl(void) {
    const char *name = "/datarouter-active-replace-test";
    unlink_shm(name);
    pid_t sw = start_switch(name, 5000U);
    pid_t dead_core = start_logged_service_wait(name, "Core");
    stop_logged_service(dead_core);

    datarouter_options opts;
    opts.shm_name = name;
    opts.timeout_ms = 1000U;

    datarouter_port *core_again = NULL;
    require_int(datarouter_open(&opts, &core_again) == DATAROUTER_OK, "open replacement core");
    char err[128];
    int rc = datarouter_logon(core_again, "Core", err, sizeof(err));
    require_int(rc == DATAROUTER_ERR_BUSY, "active Core slot is not replaced before ttl");

    datarouter_close(core_again);
    stop_switch(sw);
    unlink_shm(name);
}

static void test_snapshot_reports_active_services_without_pid_cleanup(void) {
    const char *name = "/datarouter-dead-snapshot-test";
    unlink_shm(name);
    pid_t sw = start_switch(name, 5000U);
    pid_t dead_core = start_logged_service_wait(name, "Core");

    datarouter_options opts;
    opts.shm_name = name;
    opts.timeout_ms = 1000U;

    datarouter_port *ml = NULL;
    require_int(datarouter_open(&opts, &ml) == DATAROUTER_OK, "open snapshot ml");
    logon(ml, "MLHelper");

    stop_logged_service(dead_core);

    datarouter_registry_snapshot_entry snapshot[4];
    size_t snapshot_count = 0U;
    require_int(
        datarouter_registry_snapshot(ml, snapshot, 4U, &snapshot_count) == DATAROUTER_OK,
        "dead registry snapshot succeeds"
    );
    require_int(snapshot_count == 2U, "registry snapshot keeps active services until ttl");
    require_int(strcmp(snapshot[0].service_name, "Core") == 0, "registry snapshot keeps active dead-pid service");
    require_int(strcmp(snapshot[1].service_name, "MLHelper") == 0, "registry snapshot keeps live service");

    datarouter_close(ml);
    stop_switch(sw);
    unlink_shm(name);
}

static void test_send_to_active_dead_pid_service_uses_registry_state(void) {
    const char *name = "/datarouter-dead-send-test";
    unlink_shm(name);
    pid_t sw = start_switch(name, 5000U);
    pid_t dead_core = start_logged_service_wait(name, "Core");

    datarouter_options opts;
    opts.shm_name = name;
    opts.timeout_ms = 1000U;

    datarouter_port *ml = NULL;
    require_int(datarouter_open(&opts, &ml) == DATAROUTER_OK, "open dead send ml");
    logon(ml, "MLHelper");

    stop_logged_service(dead_core);
    const char payload[] = "ping";
    require_int(
        datarouter_send(ml, "Core", payload, sizeof(payload)) == DATAROUTER_OK,
        "send to active dead-pid service succeeds before ttl"
    );

    datarouter_port *core_again = NULL;
    require_int(datarouter_open(&opts, &core_again) == DATAROUTER_OK, "open dead send replacement core");
    char err[128];
    int rc = datarouter_logon(core_again, "Core", err, sizeof(err));
    require_int(rc == DATAROUTER_ERR_BUSY, "active dead-pid service prevents replacement before ttl");

    datarouter_close(core_again);
    datarouter_close(ml);
    stop_switch(sw);
    unlink_shm(name);
}

static void test_router_routes_active_entries_without_pid_liveness(void) {
    size_t size = datarouter_shared_memory_size(4U);
    datarouter_shared_memory *shm = (datarouter_shared_memory *)calloc(1U, size);
    require_int(shm != NULL, "allocate active pid shm");
    require_int(datarouter_init_shared_memory(shm, 4U) == DATAROUTER_OK, "init active pid shm");
    activate_test_entry_pid(shm, 0U, "Core", getpid());
    activate_test_entry_pid(shm, 1U, "MLHelper", (pid_t)99999998);

    write_test_frame(shm, 1U, shm->control.registry[0].route_id, "ping");
    datarouter_scheduler_mark_tx_ready(shm, 1U);

    datarouter_route_stats stats;
    memset(&stats, 0, sizeof(stats));
    require_int(datarouter_route_frames_stats(shm, &stats) == 1U, "router routes active dead-pid frame");
    require_int(stats.routed == 1U, "router active dead-pid routed stat");
    require_int(stats.not_found == 0U, "router active dead-pid no not_found");

    datarouter_port core;
    init_test_port(&core, shm, 0U, "Core");

    char received[32];
    datarouter_message_info info;
    require_int(datarouter_recv(&core, received, sizeof(received), &info) == DATAROUTER_OK, "active dead-pid recv");
    require_int(strcmp(info.source, "MLHelper") == 0, "active dead-pid source");
    require_int(strcmp(received, "ping") == 0, "active dead-pid payload");
    free(shm);
}

static void test_ready_sources_are_fair(void) {
    size_t size = datarouter_shared_memory_size(4U);
    datarouter_shared_memory *shm = (datarouter_shared_memory *)calloc(1U, size);
    require_int(shm != NULL, "allocate fair shm");
    require_int(datarouter_init_shared_memory(shm, 4U) == DATAROUTER_OK, "init fair shm");
    activate_test_entry(shm, 0U, "Core");
    activate_test_entry(shm, 1U, "MLHelper");
    activate_test_entry(shm, 2U, "ERP");

    for (uint32_t i = 0U; i < 4U; i++) {
        write_test_frame(shm, 1U, shm->control.registry[0].route_id, "ml");
    }
    write_test_frame(shm, 2U, shm->control.registry[0].route_id, "erp");
    datarouter_scheduler_mark_tx_ready(shm, 1U);
    datarouter_scheduler_mark_tx_ready(shm, 2U);

    require_int(datarouter_route_frames(shm) == 5U, "fair router routes all frames");
    datarouter_port core;
    init_test_port(&core, shm, 0U, "Core");

    char received[32];
    datarouter_message_info info;
    require_int(datarouter_recv(&core, received, sizeof(received), &info) == DATAROUTER_OK, "fair recv first");
    require_int(strcmp(info.source, "MLHelper") == 0, "fair first source");
    require_int(datarouter_recv(&core, received, sizeof(received), &info) == DATAROUTER_OK, "fair recv second");
    require_int(strcmp(info.source, "ERP") == 0, "fair second source is not starved");
    free(shm);
}

static void test_drain_batches_complete_frames(void) {
    size_t size = datarouter_shared_memory_size(4U);
    datarouter_shared_memory *shm = (datarouter_shared_memory *)calloc(1U, size);
    require_int(shm != NULL, "allocate drain shm");
    require_int(datarouter_init_shared_memory(shm, 4U) == DATAROUTER_OK, "init drain shm");
    activate_test_entry(shm, 0U, "Core");
    activate_test_entry(shm, 1U, "MLHelper");
    activate_test_entry(shm, 2U, "ERP");

    write_test_frame(shm, 1U, shm->control.registry[0].route_id, "ml-1");
    write_test_frame(shm, 2U, shm->control.registry[0].route_id, "erp-1");
    write_test_frame(shm, 1U, shm->control.registry[0].route_id, "ml-2");
    datarouter_scheduler_mark_tx_ready(shm, 1U);
    datarouter_scheduler_mark_tx_ready(shm, 2U);
    require_int(datarouter_route_frames(shm) == 3U, "drain router routes all frames");

    datarouter_port core;
    init_test_port(&core, shm, 0U, "Core");
    uint8_t drain_buffer[DATAROUTER_RING_CAPACITY - DATAROUTER_FRAME_HEADER_SIZE];
    core.drain_buffer = drain_buffer;
    core.drain_buffer_capacity = sizeof(drain_buffer);

    drain_records records;
    memset(&records, 0, sizeof(records));
    size_t drained = 0U;
    int rc = datarouter_drain(&core, 200U, 2U, record_drain_callback, &records, &drained);
    require_int(rc == DATAROUTER_OK, "drain first batch succeeds");
    require_int(drained == 2U && records.count == 2U, "drain honors max frames");
    require_int(strcmp(records.sources[0], "MLHelper") == 0, "drain first source");
    require_int(strcmp(records.payloads[0], "ml-1") == 0, "drain first payload");
    require_int(strcmp(records.sources[1], "ERP") == 0, "drain second source");
    require_int(strcmp(records.payloads[1], "erp-1") == 0, "drain second payload");

    memset(&records, 0, sizeof(records));
    rc = datarouter_drain(&core, 200U, 8U, record_drain_callback, &records, &drained);
    require_int(rc == DATAROUTER_OK, "drain second batch succeeds");
    require_int(drained == 1U && records.count == 1U, "drain reads remaining frame");
    require_int(strcmp(records.sources[0], "MLHelper") == 0, "drain remaining source");
    require_int(strcmp(records.payloads[0], "ml-2") == 0, "drain remaining payload");

    uint64_t started = datarouter_now_ns();
    rc = datarouter_drain(&core, 50U, 8U, record_drain_callback, &records, &drained);
    uint64_t elapsed_us = (datarouter_now_ns() - started) / 1000ULL;
    require_int(rc == DATAROUTER_ERR_AGAIN, "empty drain returns again");
    require_int(drained == 0U, "empty drain does not report frames");
    require_int(elapsed_us < 5000ULL, "empty drain is bounded");
    free(shm);
}

static void test_stale_target_route_is_dropped_after_slot_reuse(void) {
    size_t size = datarouter_shared_memory_size(4U);
    datarouter_shared_memory *shm = (datarouter_shared_memory *)calloc(1U, size);
    require_int(shm != NULL, "allocate stale route shm");
    require_int(datarouter_init_shared_memory(shm, 4U) == DATAROUTER_OK, "init stale route shm");
    activate_test_entry(shm, 0U, "Core");
    activate_test_entry(shm, 1U, "MLHelper");
    uint32_t old_core_route = shm->control.registry[0].route_id;

    datarouter_registry_release_slot(shm, 0U);
    activate_test_entry(shm, 0U, "CMS");
    require_int(shm->control.registry[0].route_id != old_core_route, "reused target route changes");

    write_test_frame(shm, 1U, old_core_route, "stale-target");
    datarouter_scheduler_mark_tx_ready(shm, 1U);

    datarouter_route_stats stats;
    memset(&stats, 0, sizeof(stats));
    require_int(datarouter_route_frames_stats(shm, &stats) == 0U, "stale target frame is not routed");
    require_int(stats.not_found == 1U, "stale target increments not_found");
    require_int(datarouter_ring_used(datarouter_rx_channel(shm, 0U)) == 0U, "reused target rx remains empty");
    require_int(datarouter_ring_used(datarouter_tx_channel(shm, 1U)) == 0U, "stale target frame is discarded");
    free(shm);
}

static void test_ttl_reuse(void) {
    const char *name = "/datarouter-ttl-test";
    unlink_shm(name);
    pid_t sw = start_switch(name, 100U);

    datarouter_options opts;
    opts.shm_name = name;
    opts.timeout_ms = 1000U;

    datarouter_port *core = NULL;
    datarouter_port *ml = NULL;
    datarouter_port *erp = NULL;
    datarouter_port *cms = NULL;
    datarouter_port *core_again = NULL;
    require_int(datarouter_open(&opts, &core) == DATAROUTER_OK, "open core ttl");
    require_int(datarouter_open(&opts, &ml) == DATAROUTER_OK, "open ml ttl");
    require_int(datarouter_open(&opts, &erp) == DATAROUTER_OK, "open erp ttl");
    require_int(datarouter_open(&opts, &cms) == DATAROUTER_OK, "open cms ttl");
    require_int(datarouter_open(&opts, &core_again) == DATAROUTER_OK, "open core_again ttl");
    logon(core, "Core");
    logon(ml, "MLHelper");
    logon(erp, "ERP");
    logon(cms, "CMS");
    uint32_t old_core_route = core->route_id;

    datarouter_sleep_us(350000U);
    char err[128];
    int rc = datarouter_logon(core_again, "Core", err, sizeof(err));
    require_int(rc == DATAROUTER_OK, "ttl releases Core slot for reuse");
    require_int(core_again->route_id != old_core_route, "ttl reused slot receives new route id");

    datarouter_close(core_again);
    datarouter_close(cms);
    datarouter_close(erp);
    datarouter_close(ml);
    datarouter_close(core);
    stop_switch(sw);
    unlink_shm(name);
}

static void test_dead_process_reap_reuses_slot_without_waiting_for_ttl(void) {
    const char *name = "/datarouter-dead-process-reap-test";
    unlink_shm(name);
    pid_t sw = start_switch_reaping(name, 30000U);
    pid_t victim = start_logged_service_wait(name, "MLHelper");
    stop_logged_service(victim);

    datarouter_options opts = { .shm_name = name, .timeout_ms = 1000U };
    datarouter_port *core = NULL;
    datarouter_port *replacement = NULL;
    require_int(datarouter_open(&opts, &core) == DATAROUTER_OK, "open core after dead process");
    logon(core, "Core");
    require_int(datarouter_open(&opts, &replacement) == DATAROUTER_OK, "open replacement after dead process");
    char err[128];
    int rc = DATAROUTER_ERR_BUSY;
    for (uint32_t attempt = 0U; attempt < 100U && rc == DATAROUTER_ERR_BUSY; attempt++) {
        rc = datarouter_logon(replacement, "MLHelper", err, sizeof(err));
        if (rc == DATAROUTER_ERR_BUSY) {
            datarouter_sleep_us(10000U);
        }
    }
    require_int(rc == DATAROUTER_OK, "dead process is reaped before 30 second ttl");
    datarouter_transport_stats stats;
    require_int(
        datarouter_transport_stats_snapshot(core, &stats) == DATAROUTER_OK &&
        stats.dead_process_reaps >= 1U && stats.transport_generation != 0U,
        "dead process and transport generation stats are exposed"
    );
    datarouter_close(replacement);
    datarouter_close(core);
    stop_switch(sw);
}

static void test_stale_self_slot_reuse_returns_not_found(void) {
    const char *name = "/datarouter-stale-self-test";
    unlink_shm(name);
    pid_t sw = start_switch(name, 100U);

    datarouter_options opts;
    opts.shm_name = name;
    opts.timeout_ms = 1000U;

    datarouter_port *core = NULL;
    datarouter_port *ml = NULL;
    datarouter_port *erp = NULL;
    require_int(datarouter_open(&opts, &core) == DATAROUTER_OK, "open stale core");
    require_int(datarouter_open(&opts, &ml) == DATAROUTER_OK, "open stale ml");
    require_int(datarouter_open(&opts, &erp) == DATAROUTER_OK, "open stale erp");
    logon(core, "Core");
    logon(ml, "MLHelper");
    uint32_t old_core_route = core->route_id;

    datarouter_sleep_us(350000U);
    logon(erp, "ERP");
    require_int(erp->slot_index == core->slot_index, "erp reuses stale core slot");
    require_int(erp->route_id != old_core_route, "erp route differs from stale core route");

    const char payload[] = "stale-self";
    char received[32];
    datarouter_message_info info;
    require_int(datarouter_touch(core) == DATAROUTER_ERR_NOT_FOUND, "stale touch returns not_found");
    require_int(datarouter_poll(core) == DATAROUTER_ERR_NOT_FOUND, "stale poll returns not_found");
    require_int(
        datarouter_recv(core, received, sizeof(received), &info) == DATAROUTER_ERR_NOT_FOUND,
        "stale recv returns not_found"
    );
    require_int(datarouter_wait(core, 1U) == DATAROUTER_ERR_NOT_FOUND, "stale wait returns not_found");
    require_int(
        datarouter_send(core, "ERP", payload, sizeof(payload)) == DATAROUTER_ERR_NOT_FOUND,
        "stale send returns not_found"
    );

    datarouter_close(core);
    datarouter_registry_snapshot_entry snapshot[4];
    size_t snapshot_count = 0U;
    require_int(
        datarouter_registry_snapshot(erp, snapshot, 4U, &snapshot_count) == DATAROUTER_OK,
        "stale close does not release reused slot"
    );
    require_int(snapshot_count == 1U, "only erp remains after stale close");
    require_int(strcmp(snapshot[0].service_name, "ERP") == 0, "erp remains registered");

    datarouter_close(erp);
    datarouter_close(ml);
    stop_switch(sw);
    unlink_shm(name);
}

static void test_idle_wait_keeps_service_registered(void) {
    const char *name = "/datarouter-idle-wait-test";
    unlink_shm(name);
    pid_t sw = start_switch(name, 100U);

    datarouter_options opts;
    opts.shm_name = name;
    opts.timeout_ms = 1000U;

    datarouter_port *core = NULL;
    datarouter_port *ml = NULL;
    datarouter_port *core_again = NULL;
    require_int(datarouter_open(&opts, &core) == DATAROUTER_OK, "open idle core");
    require_int(datarouter_open(&opts, &ml) == DATAROUTER_OK, "open idle ml");
    require_int(datarouter_open(&opts, &core_again) == DATAROUTER_OK, "open idle core again");
    logon(core, "Core");
    logon(ml, "MLHelper");

    for (int i = 0; i < 8; i++) {
        require_int(datarouter_wait(core, 30U) == DATAROUTER_ERR_TIMEOUT, "idle wait times out");
        datarouter_sleep_us(20000U);
    }

    char err[128];
    int rc = datarouter_logon(core_again, "Core", err, sizeof(err));
    require_int(rc == DATAROUTER_ERR_BUSY, "idle wait prevents ttl reuse of live Core slot");

    datarouter_close(core_again);
    datarouter_close(ml);
    datarouter_close(core);
    stop_switch(sw);
    unlink_shm(name);
}

static void test_close_releases_service_slot(void) {
    const char *name = "/datarouter-close-release-test";
    unlink_shm(name);
    pid_t sw = start_switch(name, 5000U);

    datarouter_options opts;
    opts.shm_name = name;
    opts.timeout_ms = 1000U;

    datarouter_port *core = NULL;
    datarouter_port *core_again = NULL;
    require_int(datarouter_open(&opts, &core) == DATAROUTER_OK, "open close core");
    require_int(datarouter_open(&opts, &core_again) == DATAROUTER_OK, "open close core again");
    logon(core, "Core");
    datarouter_close(core);
    logon(core_again, "Core");

    datarouter_close(core_again);
    stop_switch(sw);
    unlink_shm(name);
}

static void test_config_allowlist_and_capacity(void) {
    const char *name = "/datarouter-config-test";
    const char *config_path = "build/switch-config.ini";
    unlink_shm(name);
    write_config(config_path, "Core,MLHelper,ERP,CMS,Audit");
    pid_t sw = start_switch_config(name, 5000U, config_path);

    datarouter_mapping mapping;
    require_int(datarouter_platform_open(name, 0U, &mapping) == DATAROUTER_OK, "open config shm mapping");
    require_int(mapping.shm->control.max_ports == 5U, "config owns active port count");
    require_int(mapping.shm->control.channel_count == 10U, "config owns channel count");
    datarouter_platform_close(&mapping);

    datarouter_options opts;
    opts.shm_name = name;
    opts.timeout_ms = 1000U;

    datarouter_port *ports[6] = {0};
    require_int(datarouter_open(&opts, &ports[0]) == DATAROUTER_OK, "open config core");
    require_int(datarouter_open(&opts, &ports[1]) == DATAROUTER_OK, "open config ml");
    require_int(datarouter_open(&opts, &ports[2]) == DATAROUTER_OK, "open config erp");
    require_int(datarouter_open(&opts, &ports[3]) == DATAROUTER_OK, "open config cms");
    require_int(datarouter_open(&opts, &ports[4]) == DATAROUTER_OK, "open config audit");
    require_int(datarouter_open(&opts, &ports[5]) == DATAROUTER_OK, "open config extra");
    logon(ports[0], "Core");
    logon(ports[1], "MLHelper");
    logon(ports[2], "ERP");
    logon(ports[3], "CMS");
    logon(ports[4], "Audit");

    char err[128];
    require_int(
        datarouter_logon(ports[5], "Extra", err, sizeof(err)) == DATAROUTER_ERR_PERMISSION,
        "config rejects unconfigured service"
    );

    for (int i = 5; i >= 0; i--) {
        datarouter_close(ports[i]);
    }
    stop_switch(sw);
    unlink_shm(name);
    remove(config_path);
}

static void test_config_is_startup_only(void) {
    const char *name = "/datarouter-startup-config-test";
    const char *config_path = "build/switch-startup-config.ini";
    unlink_shm(name);
    write_config(config_path, "Core,MLHelper");
    pid_t sw = start_switch_config(name, 5000U, config_path);
    write_config(config_path, "Core,MLHelper,Extra");

    datarouter_options opts;
    opts.shm_name = name;
    opts.timeout_ms = 1000U;

    datarouter_port *core = NULL;
    datarouter_port *ml = NULL;
    datarouter_port *extra = NULL;
    require_int(datarouter_open(&opts, &core) == DATAROUTER_OK, "open startup core");
    require_int(datarouter_open(&opts, &ml) == DATAROUTER_OK, "open startup ml");
    require_int(datarouter_open(&opts, &extra) == DATAROUTER_OK, "open startup extra");
    logon(core, "Core");
    logon(ml, "MLHelper");

    char err[128];
    require_int(
        datarouter_logon(extra, "Extra", err, sizeof(err)) == DATAROUTER_ERR_PERMISSION,
        "config file changes are not reloaded"
    );

    datarouter_close(extra);
    datarouter_close(ml);
    datarouter_close(core);
    stop_switch(sw);
    unlink_shm(name);
    remove(config_path);
}

static void test_duplicate_switch_is_rejected_and_exit_unlinks(void) {
    const char *name = "/datarouter-duplicate-switch-test";
    unlink_shm(name);
    pid_t first = start_switch(name, 5000U);
    pid_t second = fork();
    require_int(second >= 0, "fork duplicate switch");
    if (second == 0) {
        execl(
            "./build/datarouter-switch",
            "datarouter-switch",
            "--name",
            name,
            "--allow",
            "Core,MLHelper,ERP,CMS",
            (char *)NULL
        );
        _exit(127);
    }
    int status = 0;
    require_int(waitpid(second, &status, 0) == second, "wait duplicate switch");
    require_int(WIFEXITED(status) && WEXITSTATUS(status) != 0, "duplicate switch exits with error");

    datarouter_options opts = { .shm_name = name, .timeout_ms = 1000U };
    datarouter_port *core = NULL;
    require_int(datarouter_open(&opts, &core) == DATAROUTER_OK, "original switch remains usable");
    logon(core, "Core");
    datarouter_close(core);
    stop_switch(first);

    datarouter_mapping mapping;
    require_int(
        datarouter_platform_open(name, 0U, &mapping) == DATAROUTER_ERR_NOT_FOUND,
        "normal switch exit unlinks mapping"
    );
}

static void test_stale_switch_mapping_is_rebuilt(void) {
    const char *name = "/datarouter-stale-switch-mapping-test";
    unlink_shm(name);
    pid_t stale_owner = fork();
    require_int(stale_owner >= 0, "fork stale mapping owner");
    if (stale_owner == 0) {
        datarouter_mapping mapping;
        if (datarouter_platform_create(name, 0600, datarouter_shared_memory_size(4U), &mapping) !=
            DATAROUTER_OK) {
            _exit(2);
        }
        if (datarouter_init_shared_memory_with_ttl(mapping.shm, 4U, 5000U) != DATAROUTER_OK) {
            _exit(3);
        }
        datarouter_platform_close(&mapping);
        _exit(0);
    }
    int status = 0;
    require_int(waitpid(stale_owner, &status, 0) == stale_owner && WEXITSTATUS(status) == 0, "create stale mapping");

    pid_t sw = start_switch(name, 5000U);
    datarouter_options opts = { .shm_name = name, .timeout_ms = 1000U };
    datarouter_port *core = NULL;
    require_int(datarouter_open(&opts, &core) == DATAROUTER_OK, "open rebuilt stale mapping");
    logon(core, "Core");
    datarouter_close(core);
    stop_switch(sw);
}

static void test_dead_unpublished_registration_claim_is_recovered(void) {
    const char *name = "/datarouter-dead-claim-test";
    unlink_shm(name);
    pid_t sw = start_switch(name, 5000U);
    require_int(kill(sw, SIGSTOP) == 0, "pause switch for dead claim");

    pid_t claimant = fork();
    require_int(claimant >= 0, "fork dead claimant");
    if (claimant == 0) {
        datarouter_mapping mapping;
        if (datarouter_platform_open(name, 0U, &mapping) != DATAROUTER_OK) {
            _exit(2);
        }
        uint32_t uid = 0U;
        uint64_t start_id = 0ULL;
        if (datarouter_process_identity(getpid(), &uid, &start_id) != DATAROUTER_OK) {
            _exit(3);
        }
        uint32_t identity = (uint32_t)(start_id ^ (start_id >> 32U));
        if (identity == 0U) {
            identity = 1U;
        }
        uint64_t token = ((uint64_t)(uint32_t)getpid() << 32U) | identity;
        unsigned long long expected = 0ULL;
        if (!atomic_compare_exchange_strong_explicit(
                &mapping.shm->control.register_requests[0].claim_token,
                &expected,
                token,
                memory_order_acq_rel,
                memory_order_acquire
            )) {
            _exit(4);
        }
        datarouter_platform_close(&mapping);
        _exit(0);
    }
    datarouter_sleep_us(20000U);
    require_int(kill(sw, SIGCONT) == 0, "resume switch after dead claim");

    datarouter_options opts = { .shm_name = name, .timeout_ms = 2000U };
    datarouter_port *core = NULL;
    require_int(datarouter_open(&opts, &core) == DATAROUTER_OK, "open after dead claim");
    logon(core, "Core");
    datarouter_transport_stats stats;
    require_int(datarouter_transport_stats_snapshot(core, &stats) == DATAROUTER_OK, "claim recovery stats");
    for (uint32_t attempt = 0U;
         stats.registration_request_recoveries < 1U && attempt < 100U;
         attempt++) {
        datarouter_sleep_us(1000U);
        require_int(
            datarouter_transport_stats_snapshot(core, &stats) == DATAROUTER_OK,
            "zombie claim recovery stats"
        );
    }
    require_int(stats.registration_request_recoveries >= 1U, "zombie claim recovery counted");
    int status = 0;
    require_int(
        waitpid(claimant, &status, 0) == claimant && WEXITSTATUS(status) == 0,
        "zombie claimant reaped after recovery"
    );
    datarouter_close(core);
    stop_switch(sw);
}

static void test_alive_unpublished_claim_does_not_block_other_mailboxes(void) {
    const char *name = "/datarouter-live-claim-test";
    unlink_shm(name);
    pid_t sw = start_switch(name, 5000U);
    datarouter_mapping mapping;
    require_int(datarouter_platform_open(name, 0U, &mapping) == DATAROUTER_OK, "open live claim mapping");

    uint32_t uid = 0U;
    uint64_t start_id = 0ULL;
    require_int(
        datarouter_process_identity(getpid(), &uid, &start_id) == DATAROUTER_OK,
        "live claimant identity"
    );
    uint32_t identity = (uint32_t)(start_id ^ (start_id >> 32U));
    if (identity == 0U) {
        identity = 1U;
    }
    unsigned long long token = ((uint64_t)(uint32_t)getpid() << 32U) | identity;
    unsigned long long empty = 0ULL;
    require_int(
        atomic_compare_exchange_strong_explicit(
            &mapping.shm->control.register_requests[0].claim_token,
            &empty,
            token,
            memory_order_acq_rel,
            memory_order_acquire
        ),
        "reserve stalled live mailbox"
    );

    datarouter_options opts = { .shm_name = name, .timeout_ms = 1000U };
    datarouter_port *core = NULL;
    require_int(datarouter_open(&opts, &core) == DATAROUTER_OK, "open beside stalled mailbox");
    logon(core, "Core");
    require_int(
        atomic_load_explicit(
            &mapping.shm->control.register_requests[0].claim_token,
            memory_order_acquire
        ) == token,
        "switch leaves live unpublished claim owned"
    );
    atomic_store_explicit(
        &mapping.shm->control.register_requests[0].claim_token,
        0ULL,
        memory_order_release
    );
    datarouter_close(core);
    datarouter_platform_close(&mapping);
    stop_switch(sw);
}

static void test_registration_timeout_releases_own_request(void) {
    const char *name = "/datarouter-registration-timeout-test";
    unlink_shm(name);
    pid_t sw = start_switch(name, 5000U);
    datarouter_options opts = { .shm_name = name, .timeout_ms = 20U };
    datarouter_port *core = NULL;
    require_int(datarouter_open(&opts, &core) == DATAROUTER_OK, "open timeout port");
    require_int(kill(sw, SIGSTOP) == 0, "pause switch for registration timeout");
    char err[128];
    require_int(
        datarouter_logon(core, "Core", err, sizeof(err)) == DATAROUTER_ERR_TIMEOUT,
        "registration times out while switch paused"
    );
    require_int(kill(sw, SIGCONT) == 0, "resume switch after timeout");
    core->timeout_ms = 1000U;
    logon(core, "Core");
    datarouter_close(core);
    stop_switch(sw);
}

static void test_full_receiver_wakes_blocked_sender(void) {
    size_t size = datarouter_shared_memory_size(4U);
    datarouter_shared_memory *shm = (datarouter_shared_memory *)calloc(1U, size);
    require_int(shm != NULL, "allocate full recovery shm");
    require_int(datarouter_init_shared_memory(shm, 4U) == DATAROUTER_OK, "init full recovery shm");
    activate_test_entry(shm, 0U, "Core");
    activate_test_entry(shm, 1U, "MLHelper");
    activate_test_entry(shm, 2U, "ERP");

    size_t large_len = DATAROUTER_RING_CAPACITY - DATAROUTER_FRAME_HEADER_SIZE;
    uint8_t *large = (uint8_t *)calloc(1U, large_len);
    uint8_t header[DATAROUTER_FRAME_HEADER_SIZE];
    require_int(large != NULL, "allocate full recovery payload");
    encode_test_header(header, shm->control.registry[2].route_id, (uint32_t)large_len);
    require_int(
        datarouter_ring_write_parts(
            datarouter_rx_channel(shm, 0U),
            header,
            sizeof(header),
            large,
            large_len
        ) == DATAROUTER_OK,
        "fill receiver ring with complete frame"
    );
    write_test_frame(shm, 1U, shm->control.registry[0].route_id, "after-full");
    datarouter_scheduler_mark_tx_ready(shm, 1U);
    datarouter_route_stats stats = {0};
    uint64_t started = datarouter_now_ns();
    require_int(datarouter_route_frames_stats(shm, &stats) == 0U, "full receiver does not route frame");
    require_int(datarouter_now_ns() - started < 100000000ULL, "full route attempt is bounded");
    require_int(stats.full == 1U, "full route counted once without busy loop");

    datarouter_port core;
    init_test_port(&core, shm, 0U, "Core");
    datarouter_message_info info;
    require_int(datarouter_recv(&core, large, large_len, &info) == DATAROUTER_OK, "receiver frees full ring");
    require_int(datarouter_route_frames(shm) == 1U, "blocked sender resumes after receiver frees space");
    char received[32];
    require_int(datarouter_recv(&core, received, sizeof(received), &info) == DATAROUTER_OK, "receive resumed frame");
    require_int(strcmp(received, "after-full") == 0, "resumed frame payload preserved");
    require_int(
        atomic_load_explicit(&shm->control.blocked_wakeups, memory_order_relaxed) >= 1ULL,
        "blocked wakeup counted"
    );
    free(large);
    free(shm);
}

#ifdef DATAROUTER_TEST_HOOKS
static datarouter_port *published_frame_target = NULL;
static int published_frame_recv_rc = DATAROUTER_ERR_INTERNAL;
static char published_frame_payload[64];
static datarouter_message_info published_frame_info;

static void consume_frame_immediately_after_publish(
    datarouter_shared_memory *shm,
    uint32_t source_route,
    uint32_t target_slot
) {
    (void)shm;
    (void)source_route;
    require_int(published_frame_target != NULL, "published frame target configured");
    require_int(
        published_frame_target->slot_index == target_slot,
        "published frame target slot matches"
    );
    published_frame_recv_rc = datarouter_recv(
        published_frame_target,
        published_frame_payload,
        sizeof(published_frame_payload),
        &published_frame_info
    );
}

static void test_inflight_is_accounted_before_rx_publish(void) {
    size_t size = datarouter_shared_memory_size(4U);
    datarouter_shared_memory *shm = (datarouter_shared_memory *)calloc(1U, size);
    require_int(shm != NULL, "allocate inflight publication shm");
    require_int(
        datarouter_init_shared_memory(shm, 4U) == DATAROUTER_OK,
        "init inflight publication shm"
    );
    activate_test_entry(shm, 0U, "Source");
    activate_test_entry(shm, 1U, "Target");

    datarouter_port target;
    init_test_port(&target, shm, 1U, "Target");
    published_frame_target = &target;
    published_frame_recv_rc = DATAROUTER_ERR_INTERNAL;
    memset(published_frame_payload, 0, sizeof(published_frame_payload));
    memset(&published_frame_info, 0, sizeof(published_frame_info));

    write_test_frame(
        shm,
        0U,
        atomic_load_explicit(&shm->control.registry[1].route_id, memory_order_acquire),
        "publish-race"
    );
    datarouter_scheduler_mark_tx_ready(shm, 0U);
    datarouter_test_after_rx_publish = consume_frame_immediately_after_publish;
    require_int(datarouter_route_frames(shm) == 1U, "route frame consumed inside publish hook");
    datarouter_test_after_rx_publish = NULL;
    published_frame_target = NULL;

    require_int(published_frame_recv_rc == DATAROUTER_OK, "published frame received in race window");
    require_int(strcmp(published_frame_payload, "publish-race") == 0, "published payload preserved");
    require_int(strcmp(published_frame_info.source, "Source") == 0, "published source preserved");
    require_int(
        atomic_load_explicit(
            &shm->control.inflight_frames[0U][1U],
            memory_order_acquire
        ) == 0U,
        "immediate receive cannot strand inflight accounting"
    );
    require_int(datarouter_registry_release_slot(shm, 0U), "source releases after immediate receive");
    free(shm);
}

static void free_target_before_block_registration(
    datarouter_shared_memory *shm,
    uint32_t target_slot
) {
    datarouter_ring_reset(datarouter_rx_channel(shm, target_slot));
    (void)datarouter_scheduler_wake_blocked(shm);
}

static void test_full_prepare_to_wait_closes_lost_wakeup_window(void) {
    size_t size = datarouter_shared_memory_size(4U);
    datarouter_shared_memory *shm = (datarouter_shared_memory *)calloc(1U, size);
    require_int(shm != NULL, "allocate full race shm");
    require_int(datarouter_init_shared_memory(shm, 4U) == DATAROUTER_OK, "init full race shm");
    activate_test_entry(shm, 0U, "Target");
    activate_test_entry(shm, 1U, "Source");

    uint8_t *fill = (uint8_t *)calloc(1U, DATAROUTER_RING_CAPACITY);
    require_int(fill != NULL, "allocate full race fill");
    require_int(
        datarouter_ring_write(
            datarouter_rx_channel(shm, 0U),
            fill,
            DATAROUTER_RING_CAPACITY
        ) == DATAROUTER_OK,
        "fill target before race"
    );
    free(fill);
    write_test_frame(
        shm,
        1U,
        atomic_load_explicit(&shm->control.registry[0].route_id, memory_order_acquire),
        "after-raced-wakeup"
    );
    datarouter_scheduler_mark_tx_ready(shm, 1U);
    datarouter_test_after_full_before_block = free_target_before_block_registration;
    datarouter_route_stats stats = {0};
    require_int(
        datarouter_route_frames_stats(shm, &stats) == 1U,
        "space freed before blocked registration is rechecked"
    );
    datarouter_test_after_full_before_block = NULL;
    require_int(stats.full == 0U && stats.routed == 1U, "raced wakeup routes without full stall");
    require_int(
        !datarouter_scheduler_has_tx_blocked(shm),
        "raced wakeup leaves no permanently blocked sender"
    );
    free(shm);
}
#endif

static void test_route_budget_leaves_work_ready(void) {
    size_t size = datarouter_shared_memory_size(4U);
    datarouter_shared_memory *shm = (datarouter_shared_memory *)calloc(1U, size);
    require_int(shm != NULL, "allocate route budget shm");
    require_int(datarouter_init_shared_memory(shm, 4U) == DATAROUTER_OK, "init route budget shm");
    activate_test_entry(shm, 0U, "Core");
    activate_test_entry(shm, 1U, "MLHelper");
    for (uint32_t i = 0U; i < 20U; i++) {
        write_test_frame(shm, 1U, shm->control.registry[0].route_id, "budget");
    }
    datarouter_scheduler_mark_tx_ready(shm, 1U);
    require_int(
        datarouter_route_frames_budget(shm, NULL, 7U, 0U) == 7U,
        "route frame budget is enforced"
    );
    require_int(
        (atomic_load_explicit(&shm->control.tx_ready_mask, memory_order_acquire) & (1ULL << 1U)) != 0ULL,
        "budget exhaustion keeps source ready"
    );
    require_int(datarouter_route_frames(shm) == 13U, "remaining budgeted frames route later");
    free(shm);
}

static void test_slot_release_waits_for_pinned_io_and_drains_published_tx(void) {
    size_t size = datarouter_shared_memory_size(4U);
    datarouter_shared_memory *shm = (datarouter_shared_memory *)calloc(1U, size);
    require_int(shm != NULL, "allocate lifecycle shm");
    require_int(datarouter_init_shared_memory(shm, 4U) == DATAROUTER_OK, "init lifecycle shm");
    activate_test_entry(shm, 0U, "Source");
    activate_test_entry(shm, 1U, "Target");

    datarouter_registry_entry *source = &shm->control.registry[0];
    uint32_t old_route = atomic_load_explicit(&source->route_id, memory_order_acquire);
    require_int(
        datarouter_registry_pin_self(
            source,
            getpid(),
            getuid(),
            atomic_load_explicit(&source->process_start_id, memory_order_acquire),
            old_route,
            "Source"
        ),
        "pin old source io"
    );
    require_int(!datarouter_registry_release_slot(shm, 0U), "release defers while io pinned");
    require_int(
        atomic_load_explicit(&source->state, memory_order_acquire) == DATAROUTER_PORT_RELEASING,
        "pinned source enters releasing"
    );
    write_test_frame(
        shm,
        0U,
        atomic_load_explicit(&shm->control.registry[1].route_id, memory_order_acquire),
        "old-owner"
    );
    datarouter_registry_unpin(source);
    require_int(datarouter_registry_release_slot(shm, 0U), "release completes after unpin");
    require_int(datarouter_ring_used(datarouter_tx_channel(shm, 0U)) == 0U, "release resets old tx");

    activate_test_entry(shm, 0U, "Replacement");
    require_int(
        !datarouter_registry_pin_self(
            source,
            getpid(),
            getuid(),
            atomic_load_explicit(&source->process_start_id, memory_order_acquire),
            old_route,
            "Source"
        ),
        "old owner cannot pin reused slot"
    );

    /* A normal unregister drains already-published TX and retains source
     * metadata until the target consumes the delivered frame. */
    activate_test_entry(shm, 2U, "Closing");
    uint32_t target_route = atomic_load_explicit(
        &shm->control.registry[1].route_id,
        memory_order_acquire
    );
    write_test_frame(shm, 2U, target_route, "drain-before-close");
    require_int(datarouter_registry_begin_drain(shm, 2U), "begin source drain");
    datarouter_registry_cleanup_releasing(shm);
    datarouter_route_stats stats = {0};
    require_int(datarouter_route_frames_stats(shm, &stats) == 1U, "draining source routed");
    datarouter_registry_cleanup_releasing(shm);
    require_int(
        atomic_load_explicit(&shm->control.registry[2].state, memory_order_acquire) ==
            DATAROUTER_PORT_RELEASING,
        "source metadata remains while delivered frame is in flight"
    );

    datarouter_port target;
    init_test_port(&target, shm, 1U, "Target");
    char payload[64];
    datarouter_message_info info;
    require_int(
        datarouter_recv(&target, payload, sizeof(payload), &info) == DATAROUTER_OK,
        "target receives drained frame"
    );
    require_int(strcmp(info.source, "Closing") == 0, "drained frame keeps source identity");
    require_int(strcmp(payload, "drain-before-close") == 0, "drained frame keeps payload");
    datarouter_registry_cleanup_releasing(shm);
    require_int(
        atomic_load_explicit(&shm->control.registry[2].state, memory_order_acquire) ==
            DATAROUTER_PORT_EMPTY,
        "source releases after delivered frame consumption"
    );
    free(shm);
}

static void test_abandoned_successful_registration_rolls_back_slot(void) {
    size_t size = datarouter_shared_memory_size(4U);
    datarouter_shared_memory *shm = (datarouter_shared_memory *)calloc(1U, size);
    require_int(shm != NULL, "allocate abandoned registration shm");
    require_int(
        datarouter_init_shared_memory(shm, 4U) == DATAROUTER_OK,
        "init abandoned registration shm"
    );
    activate_test_entry(shm, 0U, "Ghost");
    datarouter_register_request *request = &shm->control.register_requests[0U];
    atomic_store_explicit(&request->claim_token, 42ULL, memory_order_release);
    atomic_store_explicit(&request->state, DATAROUTER_REG_ABANDONED, memory_order_release);

    require_int(
        datarouter_registry_publish_control_response(
            shm,
            request,
            DATAROUTER_REG_ACCEPTED,
            DATAROUTER_REG_OP_REGISTER,
            DATAROUTER_OK,
            0U
        ) == 0,
        "abandoned response is not published"
    );
    require_int(
        atomic_load_explicit(&request->state, memory_order_acquire) == DATAROUTER_REG_EMPTY &&
            atomic_load_explicit(&request->claim_token, memory_order_acquire) == 0ULL,
        "abandoned mailbox is released"
    );
    require_int(
        atomic_load_explicit(&shm->control.registry[0U].state, memory_order_acquire) ==
            DATAROUTER_PORT_EMPTY,
        "late successful registration leaves no ghost slot"
    );
    free(shm);
}

static void test_unlogged_port_cannot_be_inherited_across_process_identity(void) {
    size_t size = datarouter_shared_memory_size(4U);
    datarouter_shared_memory *shm = (datarouter_shared_memory *)calloc(1U, size);
    require_int(shm != NULL, "allocate inherited port shm");
    require_int(
        datarouter_init_shared_memory(shm, 4U) == DATAROUTER_OK,
        "init inherited port shm"
    );
    datarouter_port inherited;
    memset(&inherited, 0, sizeof(inherited));
    inherited.fd = -1;
    inherited.shm = shm;
    inherited.slot_index = UINT32_MAX;
    inherited.timeout_ms = 10U;
    inherited.process_start_id = shm->control.switch_start_id + 1ULL;
    inherited.switch_pid = shm->control.switch_pid;
    inherited.switch_uid = shm->control.switch_uid;
    inherited.switch_start_id = shm->control.switch_start_id;
    atomic_init(&inherited.last_switch_identity_check_ns, datarouter_now_ns());
    atomic_init(&inherited.switch_dead, 0U);

    char error[128];
    require_int(
        datarouter_logon(&inherited, "Forked", error, sizeof(error)) ==
            DATAROUTER_ERR_NOT_FOUND,
        "inherited unlogged port must be reopened"
    );
    require_int(
        strstr(error, "reopen after fork") != NULL,
        "inherited port reports process identity mismatch"
    );
    for (uint32_t i = 0U; i < DATAROUTER_REGISTRATION_REQUEST_SLOTS; i++) {
        require_int(
            atomic_load_explicit(
                &shm->control.register_requests[i].claim_token,
                memory_order_acquire
            ) == 0ULL,
            "inherited port never claims a request mailbox"
        );
    }
    free(shm);
}

static void test_nonzero_short_frames_are_isolated(void) {
    size_t size = datarouter_shared_memory_size(4U);
    datarouter_shared_memory *shm = (datarouter_shared_memory *)calloc(1U, size);
    require_int(shm != NULL, "allocate short frame shm");
    require_int(datarouter_init_shared_memory(shm, 4U) == DATAROUTER_OK, "init short frame shm");
    activate_test_entry(shm, 0U, "Core");
    activate_test_entry(shm, 1U, "Source");
    uint8_t byte = 0x5a;
    require_int(
        datarouter_ring_write(datarouter_tx_channel(shm, 1U), &byte, 1U) == DATAROUTER_OK,
        "publish short tx"
    );
    datarouter_scheduler_mark_tx_ready(shm, 1U);
    datarouter_route_stats stats = {0};
    require_int(datarouter_route_frames_stats(shm, &stats) == 0U, "short tx not routed");
    require_int(stats.protocol == 1U && stats.corrupted == 1U, "short tx counted corrupt");
    require_int(
        atomic_load_explicit(&shm->control.registry[1].state, memory_order_acquire) ==
            DATAROUTER_PORT_EMPTY,
        "short tx source isolated"
    );

    require_int(
        datarouter_ring_write(datarouter_rx_channel(shm, 0U), &byte, 1U) == DATAROUTER_OK,
        "publish short rx"
    );
    datarouter_port core;
    init_test_port(&core, shm, 0U, "Core");
    char payload[16];
    datarouter_message_info info;
    require_int(
        datarouter_recv(&core, payload, sizeof(payload), &info) == DATAROUTER_ERR_PROTOCOL,
        "short rx rejected as protocol corruption"
    );
    require_int(
        (atomic_load_explicit(&shm->control.corrupt_slot_mask, memory_order_acquire) & 1ULL) != 0ULL,
        "short rx schedules slot isolation"
    );
    free(shm);
}

int main(void) {
    test_route_and_allowlist();
#ifndef _WIN32
    test_concurrent_send_from_shared_port();
#endif
    test_wait_timeout();
    test_active_service_not_replaced_before_ttl();
    test_snapshot_reports_active_services_without_pid_cleanup();
    test_send_to_active_dead_pid_service_uses_registry_state();
    test_router_routes_active_entries_without_pid_liveness();
    test_ready_sources_are_fair();
    test_drain_batches_complete_frames();
    test_stale_target_route_is_dropped_after_slot_reuse();
    test_ttl_reuse();
    test_dead_process_reap_reuses_slot_without_waiting_for_ttl();
    test_stale_self_slot_reuse_returns_not_found();
    test_idle_wait_keeps_service_registered();
    test_close_releases_service_slot();
    test_config_allowlist_and_capacity();
    test_config_is_startup_only();
    test_duplicate_switch_is_rejected_and_exit_unlinks();
    test_stale_switch_mapping_is_rebuilt();
    test_dead_unpublished_registration_claim_is_recovered();
    test_alive_unpublished_claim_does_not_block_other_mailboxes();
    test_registration_timeout_releases_own_request();
    test_full_receiver_wakes_blocked_sender();
#ifdef DATAROUTER_TEST_HOOKS
    test_inflight_is_accounted_before_rx_publish();
    test_full_prepare_to_wait_closes_lost_wakeup_window();
#endif
    test_route_budget_leaves_work_ready();
    test_slot_release_waits_for_pinned_io_and_drains_published_tx();
    test_abandoned_successful_registration_rolls_back_slot();
    test_unlogged_port_cannot_be_inherited_across_process_identity();
    test_nonzero_short_frames_are_isolated();

    printf("test_switch ok\n");
    return 0;
}
