/* Data-plane copy-path micro-benchmark: isolates the primitives the direct
 * (unsafe) mode pays per frame — ring write (app -> RX ring) and ring read
 * (RX ring -> app) — plus plain memcpy / copy_avx2 baselines, so the asm
 * decision (mask vs AVX2 vs hand-written) is grounded in per-primitive data.
 *
 * Usage:
 *   cc -O3 -fno-strict-aliasing -DDATAROUTER_CHANNEL_SIZE=524288U \
 *      -Iinclude -Isrc -D_POSIX_C_SOURCE=200809L \
 *      tests_bench_copy.c src/ring.c src/common.c src/port.c src/registry.c \
 *      src/scheduler.c src/signal.c src/router.c src/platform_linux.c \
 *      -o /tmp/copybench -lpthread
 *   DATAROUTER_COPY=scalar /tmp/copybench
 *   DATAROUTER_COPY=avx2   /tmp/copybench
 */
#include "../src/datarouter_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static void bench(const char *label, size_t bytes, size_t iters, void (*fn)(void)) {
    double start = now_s();
    fn();
    double elapsed = now_s() - start;
    double gb_s = (double)bytes * (double)iters / elapsed / 1e9;
    printf("%-28s %8zu B x %6zu = %8.1f ms  %8.2f GiB/s\n",
           label, bytes, iters, elapsed * 1e3, gb_s);
}

static size_t g_bytes;
static size_t g_iters;
static uint8_t *g_src;
static uint8_t *g_dst;
static datarouter_channel *g_channel;

static void do_memcpy(void) {
    for (size_t i = 0; i < g_iters; i++) {
        memcpy(g_dst, g_src, g_bytes);
    }
}

static void do_ring_write(void) {
    for (size_t i = 0; i < g_iters; i++) {
        if (datarouter_ring_write(g_channel, g_src, g_bytes) != DATAROUTER_OK) {
            datarouter_ring_reset(g_channel);
        }
    }
}

static void do_ring_write_read(void) {
    for (size_t i = 0; i < g_iters; i++) {
        if (datarouter_ring_write(g_channel, g_src, g_bytes) != DATAROUTER_OK) {
            datarouter_ring_reset(g_channel);
        }
        if (datarouter_ring_read(g_channel, g_dst, g_bytes) != DATAROUTER_OK) {
            datarouter_ring_reset(g_channel);
        }
    }
}

int main(void) {
    g_bytes = 300 * 1024;
    g_iters = 20000;
    g_src = mmap(NULL, 1 << 20, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    g_dst = mmap(NULL, 1 << 20, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    g_channel = mmap(NULL, sizeof(datarouter_channel), PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (g_src == MAP_FAILED || g_dst == MAP_FAILED || g_channel == MAP_FAILED) {
        perror("mmap");
        return 1;
    }
    memset(g_src, 0xA5, 1 << 20);
    datarouter_ring_reset(g_channel);

    printf("copy strategy: %s\n", datarouter_copy_strategy());

    bench("memcpy 300KiB", g_bytes, g_iters, do_memcpy);
    bench("ring write 300KiB", g_bytes, g_iters, do_ring_write);
    bench("ring write+read 300KiB", g_bytes * 2, g_iters, do_ring_write_read);
    return 0;
}
