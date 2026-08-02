#include "../include/datarouter.h"

#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define BENCH_BURSTS 200U
#define BENCH_FRAMES_PER_BURST 256U
#define BENCH_PAYLOAD_SIZE 64U
#define BENCH_TIMEOUT_MS 1000U

typedef struct {
    uint32_t count;
    uint32_t expected_len;
} drain_state;

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

static void require_ok(int rc, const char *message) {
    if (rc != DATAROUTER_OK) {
        fprintf(stderr, "bench_burst failed: %s: %s\n", message, datarouter_strerror(rc));
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
        fprintf(stderr, "bench_burst failed: logon %s: %s (%s)\n", service, datarouter_strerror(rc), errbuf);
        exit(1);
    }
}

static int drain_callback(
    const datarouter_message_info *info,
    const void *data,
    size_t len,
    void *user_data
) {
    (void)data;
    drain_state *state = (drain_state *)user_data;
    if (!info || !state || strcmp(info->source, "BenchA") != 0 || len != state->expected_len) {
        return DATAROUTER_ERR_PROTOCOL;
    }
    state->count++;
    return DATAROUTER_OK;
}

static void send_burst(datarouter_port *from, uint8_t *payload) {
    for (uint32_t i = 0U; i < BENCH_FRAMES_PER_BURST; i++) {
        require_ok(datarouter_send(from, "BenchB", payload, BENCH_PAYLOAD_SIZE), "send burst frame");
    }
}

static uint64_t recv_burst_one_by_one(datarouter_port *to, uint8_t *buffer) {
    datarouter_message_info info;
    uint64_t started = now_ns();
    for (uint32_t i = 0U; i < BENCH_FRAMES_PER_BURST; i++) {
        require_ok(datarouter_wait(to, BENCH_TIMEOUT_MS), "wait recv frame");
        require_ok(datarouter_recv(to, buffer, BENCH_PAYLOAD_SIZE, &info), "recv frame");
        if (strcmp(info.source, "BenchA") != 0 || info.len != BENCH_PAYLOAD_SIZE) {
            fprintf(stderr, "bench_burst failed: invalid recv frame metadata\n");
            exit(1);
        }
    }
    return now_ns() - started;
}

static uint64_t recv_burst_drain(datarouter_port *to) {
    uint32_t received = 0U;
    uint64_t started = now_ns();
    while (received < BENCH_FRAMES_PER_BURST) {
        require_ok(datarouter_wait(to, BENCH_TIMEOUT_MS), "wait drain frame");
        drain_state state;
        state.count = 0U;
        state.expected_len = BENCH_PAYLOAD_SIZE;
        size_t drained = 0U;
        int rc = datarouter_drain(
            to,
            200U,
            BENCH_FRAMES_PER_BURST - received,
            drain_callback,
            &state,
            &drained
        );
        if (rc == DATAROUTER_ERR_AGAIN && drained == 0U) {
            continue;
        }
        require_ok(rc, "drain frames");
        if (drained != state.count) {
            fprintf(stderr, "bench_burst failed: drained=%zu callback_count=%u\n", drained, state.count);
            exit(1);
        }
        received += (uint32_t)drained;
    }
    return now_ns() - started;
}

static void print_result(const char *name, uint64_t *latencies, size_t count) {
    uint64_t total_ns = 0ULL;
    for (size_t i = 0U; i < count; i++) {
        total_ns += latencies[i];
    }
    qsort(latencies, count, sizeof(uint64_t), cmp_u64);
    double total_frames = (double)BENCH_BURSTS * (double)BENCH_FRAMES_PER_BURST;
    printf(
        "{\"transport\":\"datarouter\",\"mode\":\"%s\",\"payload_bytes\":%u,"
        "\"bursts\":%u,\"frames_per_burst\":%u,\"avg_frame_us\":%.3f,"
        "\"p50_burst_us\":%llu,\"p95_burst_us\":%llu,\"p99_burst_us\":%llu,"
        "\"max_burst_us\":%llu,\"throughput_msg_s\":%.3f}\n",
        name,
        BENCH_PAYLOAD_SIZE,
        BENCH_BURSTS,
        BENCH_FRAMES_PER_BURST,
        (double)total_ns / total_frames / 1000.0,
        (unsigned long long)percentile_us(latencies, count, 0.50),
        (unsigned long long)percentile_us(latencies, count, 0.95),
        (unsigned long long)percentile_us(latencies, count, 0.99),
        (unsigned long long)(latencies[count - 1U] / 1000ULL),
        total_frames / ((double)total_ns / 1000000000.0)
    );
}

int main(void) {
    const char *name = "/datarouter-burst-bench";
    unlink_shm(name);
    pid_t sw = start_switch(name);

    datarouter_options opts = {0};
    opts.shm_name = name;
    opts.timeout_ms = 3000U;

    datarouter_port *a = NULL;
    datarouter_port *b = NULL;
    for (uint32_t i = 0U; i < 100U; i++) {
        if (datarouter_open(&opts, &a) == DATAROUTER_OK) {
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

    uint8_t outbound[BENCH_PAYLOAD_SIZE];
    uint8_t inbound[BENCH_PAYLOAD_SIZE];
    memset(outbound, 0x7b, sizeof(outbound));

    uint64_t recv_latencies[BENCH_BURSTS];
    uint64_t drain_latencies[BENCH_BURSTS];

    for (uint32_t i = 0U; i < BENCH_BURSTS; i++) {
        send_burst(a, outbound);
        uint64_t elapsed = recv_burst_one_by_one(b, inbound);
        recv_latencies[i] = elapsed;

        send_burst(a, outbound);
        elapsed = recv_burst_drain(b);
        drain_latencies[i] = elapsed;
    }

    print_result("recv", recv_latencies, BENCH_BURSTS);
    print_result("drain", drain_latencies, BENCH_BURSTS);

    datarouter_close(a);
    datarouter_close(b);
    stop_switch(sw);
    unlink_shm(name);
    return 0;
}
