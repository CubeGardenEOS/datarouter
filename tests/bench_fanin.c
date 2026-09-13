/* CGSGL v2. CLI-owned C fan-in: independent source ports, validated replies. */
#define _GNU_SOURCE
#include "datarouter.h"
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

#define WARMUP 10000U
#define SAMPLES 20000U
typedef struct { uint32_t source, sequence; uint64_t sent_ns; } frame;
typedef struct {
    datarouter_port *port;
    uint32_t index;
    int cpu;
    size_t bytes;
} sender;
static pthread_barrier_t boundary;

static uint64_t now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ULL + (uint64_t)t.tv_nsec;
}
static void fail(const char *message, int code) {
    fprintf(stderr, "%s: %d\n", message, code);
    exit(1);
}
static void pin(int cpu) {
    cpu_set_t mask; CPU_ZERO(&mask); CPU_SET(cpu, &mask);
    if (sched_setaffinity(0, sizeof(mask), &mask)) fail("affinity", cpu);
}
static void send_frame(datarouter_port *port, const char *target, const void *data, size_t bytes) {
    uint64_t deadline = now_ns() + 5000000000ULL;
    for (;;) {
        int rc = datarouter_unsafe_send(port, target, data, bytes);
        if (rc == DATAROUTER_OK) return;
        if ((rc != DATAROUTER_ERR_FULL && rc != DATAROUTER_ERR_AGAIN) || now_ns() >= deadline)
            fail("send", rc);
        __asm__ __volatile__("pause" ::: "memory");
    }
}
static void receive(datarouter_port *port, void *data, size_t bytes, datarouter_message_info *info) {
    uint64_t deadline = now_ns() + 5000000000ULL;
    for (;;) {
        int rc = datarouter_recv(port, data, bytes, info);
        if (rc == DATAROUTER_OK) return;
        if (rc != DATAROUTER_ERR_AGAIN || now_ns() >= deadline) fail("receive", rc);
        rc = datarouter_wait(port, 10);
        if (rc != DATAROUTER_OK && rc != DATAROUTER_ERR_TIMEOUT) fail("wait", rc);
    }
}
static void *produce(void *opaque) {
    sender *s = opaque;
    pin(s->cpu);
    uint8_t *data = malloc(s->bytes);
    if (!data) fail("allocation", 0);
    memset(data, (int)(0xa0 + s->index), s->bytes);
    for (uint32_t i = 0; i < WARMUP + SAMPLES; i++) {
        if (i == WARMUP) pthread_barrier_wait(&boundary);
        frame item = {s->index, i, now_ns()};
        memcpy(data, &item, sizeof(item));
        send_frame(s->port, "Receiver", data, s->bytes);
        frame reply; datarouter_message_info info;
        receive(s->port, &reply, sizeof(reply), &info);
        if (info.len != sizeof(reply) || memcmp(&reply, &item, sizeof(reply))) {
            fprintf(stderr, "reply mismatch sender=%u expected_seq=%u actual_source=%u actual_seq=%u len=%zu expected_ns=%llu actual_ns=%llu\n",
                    s->index, i, reply.source, reply.sequence, info.len,
                    (unsigned long long)item.sent_ns, (unsigned long long)reply.sent_ns);
            fail("reply mismatch", 0);
        }
    }
    free(data);
    return NULL;
}
static int order_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}
static datarouter_port *open_port(const char *shm, const char *service) {
    datarouter_options options = {0}; options.shm_name = shm; options.timeout_ms = 5000;
    datarouter_port *port = NULL;
    int rc = datarouter_open(&options, &port);
    if (rc) fail("open", rc);
    char error[256]; rc = datarouter_logon(port, service, error, sizeof(error));
    if (rc) fail(error, rc);
    return port;
}
int main(int argc, char **argv) {
    fprintf(stderr, "version=%s\n", datarouter_version());
    if (argc != 5) return 2;
    unsigned count = (unsigned)strtoul(argv[2], NULL, 10);
    size_t bytes = strtoul(argv[3], NULL, 10);
    if ((count != 2 && count != 4 && count != 8) || bytes < sizeof(frame) + 1 || bytes > DATAROUTER_MAX_APP_PAYLOAD) return 2;
    int cpus[9]; unsigned ncpu = 0;
    char *list = strdup(argv[4]), *saved = NULL;
    for (char *value = strtok_r(list, ",", &saved); value; value = strtok_r(NULL, ",", &saved)) {
        if (ncpu >= 9) return 2;
        cpus[ncpu] = atoi(value);
        if (cpus[ncpu] < 0 || cpus[ncpu] >= CPU_SETSIZE) return 2;
        ncpu++;
    }
    free(list);
    if (ncpu != count + 1) return 2;
    datarouter_port *receiver = open_port(argv[1], "Receiver");
    sender senders[8]; pthread_t threads[8];
    for (unsigned i = 0; i < count; i++) {
        char service[32]; snprintf(service, sizeof(service), "Sender%u", i);
        senders[i] = (sender){open_port(argv[1], service), i, cpus[i+1], bytes};
    }
    if (pthread_barrier_init(&boundary, NULL, count+1)) return 2;
    for (unsigned i = 0; i < count; i++)
        if (pthread_create(&threads[i], NULL, produce, &senders[i])) return 2;
    pin(cpus[0]);
    uint8_t *data = malloc(bytes);
    size_t total = (size_t)count * SAMPLES, samples = 0;
    uint64_t *latency = malloc(total * sizeof(*latency));
    if (!data || !latency) return 2;
    uint32_t sequence[8] = {0}; uint64_t start = 0, elapsed = 0;
    for (size_t i = 0; i < (size_t)count*(WARMUP+SAMPLES); i++) {
        if (i == (size_t)count*WARMUP) { start = now_ns(); pthread_barrier_wait(&boundary); }
        datarouter_message_info info; receive(receiver, data, bytes, &info);
        uint64_t received = now_ns(); frame item; memcpy(&item, data, sizeof(item));
        if (info.len != bytes || item.source >= count || item.sequence != sequence[item.source]++ ||
            data[bytes-1] != (uint8_t)(0xa0+item.source) || received < item.sent_ns) fail("frame mismatch", 0);
        if (item.sequence >= WARMUP) latency[samples++] = received-item.sent_ns;
        send_frame(receiver, info.source, &item, sizeof(item));
    }
    elapsed = now_ns()-start;
    for (unsigned i = 0; i < count; i++) pthread_join(threads[i], NULL);
    if (samples != total) return 1;
    qsort(latency, total, sizeof(*latency), order_u64);
    struct rusage usage; getrusage(RUSAGE_SELF, &usage);
    printf("{\"senders\":%u,\"payload_bytes\":%zu,\"samples\":%zu,\"warmup_per_sender\":%u,\"errors\":0,"
           "\"frames_per_second\":%.3f,\"p50_us\":%.3f,\"p99_us\":%.3f,\"max_rss_kib\":%ld}\n",
           count, bytes, total, WARMUP, total*1e9/(double)elapsed,
           latency[total/2]/1000.0, latency[total*99/100]/1000.0, usage.ru_maxrss);
    for (unsigned i = 0; i < count; i++) datarouter_close(senders[i].port);
    datarouter_close(receiver); pthread_barrier_destroy(&boundary);
    free(data); free(latency);
    return 0;
}
