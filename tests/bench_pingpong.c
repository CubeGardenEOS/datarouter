#include "../src/datarouter_internal.h"

#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define BENCH_ITERATIONS 10000U
#define BENCH_WARMUP 1000U
#define BENCH_TIMEOUT_MS 1000U

static const size_t payload_sizes[] = {64U, 1024U, 65536U, 1048576U};

static datarouter_port *g_a = NULL;
static datarouter_port *g_b = NULL;
static pid_t g_switch_pid = 0;

static uint64_t now_ns(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0ULL;
    }
    return ((uint64_t)ts.tv_sec * 1000000000ULL) + (uint64_t)ts.tv_nsec;
}

static int cmp_u64(const void *a, const void *b) {
    uint64_t av = *(const uint64_t *)a;
    uint64_t bv = *(const uint64_t *)b;
    return (av > bv) - (av < bv);
}

static uint64_t percentile_us(uint64_t *values, size_t count, double pct) {
    if (count == 0U) {
        return 0ULL;
    }
    size_t idx = (size_t)(((double)(count - 1U)) * pct);
    return values[idx] / 1000ULL;
}

static void print_result(const char *transport, size_t payload, uint64_t *latencies, size_t count) {
    qsort(latencies, count, sizeof(uint64_t), cmp_u64);
    uint64_t max_us = latencies[count - 1U] / 1000ULL;
    uint64_t total_ns = 0ULL;
    for (size_t i = 0U; i < count; i++) {
        total_ns += latencies[i];
    }
    double throughput = ((double)count * 2.0) / ((double)total_ns / 1000000000.0);
    printf(
        "{\"transport\":\"%s\",\"payload_bytes\":%zu,\"iterations\":%zu,"
        "\"p50_us\":%llu,\"p95_us\":%llu,\"p99_us\":%llu,\"max_us\":%llu,"
        "\"throughput_msg_s\":%.3f}\n",
        transport,
        payload,
        count,
        (unsigned long long)percentile_us(latencies, count, 0.50),
        (unsigned long long)percentile_us(latencies, count, 0.95),
        (unsigned long long)percentile_us(latencies, count, 0.99),
        (unsigned long long)max_us,
        throughput
    );
}

static void print_skipped(const char *transport, size_t payload, const char *reason) {
    printf(
        "{\"transport\":\"%s\",\"payload_bytes\":%zu,\"iterations\":0,\"skipped\":true,\"reason\":\"%s\"}\n",
        transport,
        payload,
        reason
    );
}

static void require_ok(int rc, const char *message) {
    if (rc != DATAROUTER_OK) {
        fprintf(stderr, "bench_pingpong failed: %s: %s\n", message, datarouter_strerror(rc));
        exit(1);
    }
}

static void dump_state(uint32_t iteration, const char *message) {
    fprintf(stderr, "bench_pingpong state: iteration=%u step=%s switch_alive=%d\n", iteration, message, kill(g_switch_pid, 0) == 0);
    if (!g_a || !g_a->shm || !g_b || !g_b->shm) {
        return;
    }
    datarouter_shared_memory *shm = g_a->shm;
    fprintf(
        stderr,
        "  masks ready=0x%016llx blocked=0x%016llx a_slot=%u b_slot=%u\n",
        (unsigned long long)atomic_load_explicit(&shm->control.tx_ready_mask, memory_order_acquire),
        (unsigned long long)atomic_load_explicit(&shm->control.tx_blocked_mask, memory_order_acquire),
        g_a->slot_index,
        g_b->slot_index
    );
    fprintf(
        stderr,
        "  A rx_used=%zu tx_used=%zu B rx_used=%zu tx_used=%zu\n",
        datarouter_ring_used(datarouter_rx_channel(shm, g_a->slot_index)),
        datarouter_ring_used(datarouter_tx_channel(shm, g_a->slot_index)),
        datarouter_ring_used(datarouter_rx_channel(shm, g_b->slot_index)),
        datarouter_ring_used(datarouter_tx_channel(shm, g_b->slot_index))
    );
}

static void require_iter_ok(int rc, uint32_t iteration, const char *message) {
    if (rc != DATAROUTER_OK) {
        dump_state(iteration, message);
        fprintf(
            stderr,
            "bench_pingpong failed: iteration=%u %s: %s\n",
            iteration,
            message,
            datarouter_strerror(rc)
        );
        exit(1);
    }
}

static void unlink_shm(const char *name) {
    shm_unlink(name);
}

static pid_t start_switch(const char *name) {
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        exit(1);
    }
    if (pid == 0) {
        execl(
            "build/datarouter-switch",
            "datarouter-switch",
            "--name",
            name,
            "--allow",
            "BenchA,BenchB",
            "--ttl-ms",
            "600000",
            (char *)NULL
        );
        perror("execl datarouter-switch");
        _exit(127);
    }
    return pid;
}

static void stop_switch(pid_t pid) {
    kill(pid, SIGTERM);
    waitpid(pid, NULL, 0);
}

static void logon(datarouter_port *port, const char *service) {
    char errbuf[128];
    int rc = datarouter_logon(port, service, errbuf, sizeof(errbuf));
    if (rc != DATAROUTER_OK) {
        fprintf(stderr, "bench_pingpong failed: logon %s: %s (%s)\n", service, datarouter_strerror(rc), errbuf);
        exit(1);
    }
}

static void run_datarouter_payload(datarouter_port *a, datarouter_port *b, size_t payload_size) {
    size_t max_payload = DATAROUTER_RING_CAPACITY - DATAROUTER_FRAME_HEADER_SIZE;
    if (payload_size > max_payload) {
        print_skipped("datarouter", payload_size, "payload exceeds datarouter single-frame capacity");
        return;
    }
    uint8_t *outbound = malloc(payload_size);
    uint8_t *inbound = malloc(payload_size);
    uint64_t *latencies = calloc(BENCH_ITERATIONS, sizeof(uint64_t));
    if (!outbound || !inbound || !latencies) {
        fprintf(stderr, "bench_pingpong failed: allocation failed\n");
        exit(1);
    }
    memset(outbound, 0x5a, payload_size);
    datarouter_message_info info;

    for (uint32_t i = 0U; i < BENCH_WARMUP + BENCH_ITERATIONS; i++) {
        uint64_t started = now_ns();
        require_iter_ok(datarouter_send(a, "BenchB", outbound, payload_size), i, "send A to B");
        require_iter_ok(datarouter_wait(b, BENCH_TIMEOUT_MS), i, "wait B");
        require_iter_ok(datarouter_recv(b, inbound, payload_size, &info), i, "recv B");
        require_iter_ok(datarouter_send(b, "BenchA", inbound, payload_size), i, "send B to A");
        require_iter_ok(datarouter_wait(a, BENCH_TIMEOUT_MS), i, "wait A");
        require_iter_ok(datarouter_recv(a, inbound, payload_size, &info), i, "recv A");
        if (i >= BENCH_WARMUP) {
            latencies[i - BENCH_WARMUP] = now_ns() - started;
        }
    }
    print_result("datarouter", payload_size, latencies, BENCH_ITERATIONS);
    free(outbound);
    free(inbound);
    free(latencies);
}

static void read_full(int fd, uint8_t *buffer, size_t len) {
    size_t offset = 0U;
    while (offset < len) {
        ssize_t n = read(fd, buffer + offset, len - offset);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            perror("read socketpair");
            exit(1);
        }
        offset += (size_t)n;
    }
}

static void write_full(int fd, const uint8_t *buffer, size_t len) {
    size_t offset = 0U;
    while (offset < len) {
        ssize_t n = write(fd, buffer + offset, len - offset);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            perror("write socketpair");
            exit(1);
        }
        offset += (size_t)n;
    }
}

static void run_uds_payload(size_t payload_size) {
    int fds[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
        perror("socketpair");
        exit(1);
    }
    pid_t peer = fork();
    if (peer < 0) {
        perror("fork uds peer");
        exit(1);
    }
    if (peer == 0) {
        close(fds[0]);
        uint8_t *peer_buffer = malloc(payload_size);
        if (!peer_buffer) {
            _exit(127);
        }
        for (uint32_t i = 0U; i < BENCH_WARMUP + BENCH_ITERATIONS; i++) {
            read_full(fds[1], peer_buffer, payload_size);
            write_full(fds[1], peer_buffer, payload_size);
        }
        free(peer_buffer);
        close(fds[1]);
        _exit(0);
    }
    close(fds[1]);
    uint8_t *outbound = malloc(payload_size);
    uint8_t *inbound = malloc(payload_size);
    uint64_t *latencies = calloc(BENCH_ITERATIONS, sizeof(uint64_t));
    if (!outbound || !inbound || !latencies) {
        fprintf(stderr, "bench_pingpong failed: allocation failed\n");
        exit(1);
    }
    memset(outbound, 0xa5, payload_size);
    for (uint32_t i = 0U; i < BENCH_WARMUP + BENCH_ITERATIONS; i++) {
        uint64_t started = now_ns();
        write_full(fds[0], outbound, payload_size);
        read_full(fds[0], inbound, payload_size);
        if (i >= BENCH_WARMUP) {
            latencies[i - BENCH_WARMUP] = now_ns() - started;
        }
    }
    print_result("uds", payload_size, latencies, BENCH_ITERATIONS);
    close(fds[0]);
    waitpid(peer, NULL, 0);
    free(outbound);
    free(inbound);
    free(latencies);
}

int main(void) {
    const char *name = "/datarouter-bench";
    unlink_shm(name);
    pid_t sw = start_switch(name);
    g_switch_pid = sw;

    datarouter_options opts = {0};
    opts.shm_name = name;
    opts.timeout_ms = 3000U;

    datarouter_port *a = NULL;
    datarouter_port *b = NULL;
    for (uint32_t i = 0U; i < 100U; i++) {
        int rc = datarouter_open(&opts, &a);
        if (rc == DATAROUTER_OK) {
            break;
        }
        struct timespec delay;
        delay.tv_sec = 0;
        delay.tv_nsec = 10000000L;
        nanosleep(&delay, NULL);
    }
    require_ok(a ? DATAROUTER_OK : DATAROUTER_ERR_NOT_FOUND, "open BenchA");
    require_ok(datarouter_open(&opts, &b), "open BenchB");
    logon(a, "BenchA");
    logon(b, "BenchB");
    g_a = a;
    g_b = b;

    for (size_t i = 0U; i < sizeof(payload_sizes) / sizeof(payload_sizes[0]); i++) {
        run_datarouter_payload(a, b, payload_sizes[i]);
        run_uds_payload(payload_sizes[i]);
    }

    datarouter_close(a);
    datarouter_close(b);
    stop_switch(sw);
    unlink_shm(name);
    return 0;
}
