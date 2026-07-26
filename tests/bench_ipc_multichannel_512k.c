#define _GNU_SOURCE

#include "../include/datarouter.h"

#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifdef __linux__
#include <sched.h>
#endif

#define CHANNEL_COUNT 3U
#define KIND_GET 0U
#define KIND_POST 1U
#define KIND_WS 2U
#define DEFAULT_DURATION_S 15U
#define DEFAULT_INFLIGHT 1024U
#define DEFAULT_PAYLOAD_BYTES 1024U
#define DEFAULT_LATENCY_US 50000ULL
#define DEFAULT_TIMEOUT_MS 3000U
#define DEFAULT_RSS_LIMIT_BYTES (1024ULL * 1024ULL * 1024ULL)
#define DRAIN_DEADLINE_US 2000000ULL
#define MAX_BENCH_CPUS 256U

typedef struct {
    uint64_t seq;
    uint8_t kind;
    uint8_t reserved[7];
} bench_request;

typedef struct {
    uint64_t seq;
    uint8_t kind;
    uint8_t reserved[7];
} bench_response;

typedef struct {
    const char *name;
    const char *shm_base;
} channel_config;

typedef struct {
    long online_cpus;
    uint32_t usable_cpus;
    uint32_t spin_cpus;
    uint32_t app_cpus;
    uint32_t lane_count;
    int usable_cpu_ids[MAX_BENCH_CPUS];
    int spin_cpu_ids[MAX_BENCH_CPUS];
    int app_cpu_ids[MAX_BENCH_CPUS];
    char usable_cpu_set[1024];
    char spin_cpu_set[1024];
    char app_cpu_set[1024];
} cpu_plan;

typedef struct {
    uint32_t duration_s;
    uint32_t inflight;
    uint32_t payload_bytes;
    uint64_t latency_us;
    uint32_t timeout_ms;
    uint64_t rss_limit_bytes;
    const char *cpus;
    const char *spin_cpus_arg;
    const char *app_cpus_arg;
    const char *switch_bin;
    int get_percent;
    int post_percent;
    int ws_percent;
    int get_hit_percent;
    const char *role;
} bench_config;

typedef struct {
    uint64_t count;
    uint64_t send_errors;
    uint64_t *ipc_us;
    uint64_t ipc_count;
    size_t ipc_cap;
} channel_stats;

typedef struct {
    uint8_t kind;
    uint8_t cache_hit;
    uint8_t channel;
    uint8_t active;
    uint64_t seq;
    uint64_t started_ns;
} pending_request;

typedef struct {
    uint8_t kind;
    uint8_t cache_hit;
    uint8_t channel;
    uint64_t due_ns;
    uint64_t latency_us;
    uint64_t ipc_us;
} completion_event;

typedef struct {
    uint8_t kind;
    uint8_t cache_hit;
    uint64_t latency_us;
    uint64_t ipc_us;
} latency_sample;

typedef struct {
    latency_sample *items;
    size_t len;
    size_t cap;
} sample_store;

static const channel_config CHANNELS[CHANNEL_COUNT] = {
    {"GET_miss", "/datarouter-ipcbench-get"},
    {"POST", "/datarouter-ipcbench-post"},
    {"ws", "/datarouter-ipcbench-ws"},
};

static uint64_t now_ns(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0ULL;
    }
    return ((uint64_t)ts.tv_sec * 1000000000ULL) + (uint64_t)ts.tv_nsec;
}

static void sleep_us(uint64_t us) {
    struct timespec ts;
    ts.tv_sec = (time_t)(us / 1000000ULL);
    ts.tv_nsec = (long)(us % 1000000ULL) * 1000L;
    nanosleep(&ts, NULL);
}

static uint64_t ns_to_us(uint64_t ns) {
    return ns / 1000ULL;
}

static void die(const char *message) {
    perror(message);
    exit(1);
}

static void die_msg(const char *message) {
    fprintf(stderr, "bench_ipc_multichannel_512k failed: %s\n", message);
    exit(1);
}

static void require_ok(int rc, const char *message) {
    if (rc != DATAROUTER_OK) {
        fprintf(stderr, "bench_ipc_multichannel_512k failed: %s: %s\n", message, datarouter_strerror(rc));
        exit(1);
    }
}

static uint64_t parse_u64(const char *raw, const char *name) {
    char *end = NULL;
    errno = 0;
    uint64_t value = strtoull(raw, &end, 10);
    if (errno != 0 || !end || *end != '\0') {
        fprintf(stderr, "invalid %s: %s\n", name, raw);
        exit(2);
    }
    return value;
}

static bench_config default_config(void) {
    bench_config cfg;
    cfg.duration_s = DEFAULT_DURATION_S;
    cfg.inflight = DEFAULT_INFLIGHT;
    cfg.payload_bytes = DEFAULT_PAYLOAD_BYTES;
    cfg.latency_us = DEFAULT_LATENCY_US;
    cfg.timeout_ms = DEFAULT_TIMEOUT_MS;
    cfg.rss_limit_bytes = DEFAULT_RSS_LIMIT_BYTES;
    cfg.cpus = "auto";
    cfg.spin_cpus_arg = "";
    cfg.app_cpus_arg = "";
    cfg.switch_bin = "build-ipcbench-512k/datarouter-switch";
    cfg.get_percent = 80;
    cfg.post_percent = 5;
    cfg.ws_percent = 15;
    cfg.get_hit_percent = 95;
    cfg.role = "";
    return cfg;
}

static bench_config parse_args(int argc, char **argv) {
    bench_config cfg = default_config();
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--role") == 0 && i + 1 < argc) {
            cfg.role = argv[++i];
        } else if (strcmp(argv[i], "--duration") == 0 && i + 1 < argc) {
            cfg.duration_s = (uint32_t)parse_u64(argv[++i], "duration");
        } else if (strcmp(argv[i], "--inflight") == 0 && i + 1 < argc) {
            cfg.inflight = (uint32_t)parse_u64(argv[++i], "inflight");
        } else if (strcmp(argv[i], "--payload-bytes") == 0 && i + 1 < argc) {
            cfg.payload_bytes = (uint32_t)parse_u64(argv[++i], "payload-bytes");
        } else if (strcmp(argv[i], "--latency-us") == 0 && i + 1 < argc) {
            cfg.latency_us = parse_u64(argv[++i], "latency-us");
        } else if (strcmp(argv[i], "--timeout-ms") == 0 && i + 1 < argc) {
            cfg.timeout_ms = (uint32_t)parse_u64(argv[++i], "timeout-ms");
        } else if (strcmp(argv[i], "--rss-limit-bytes") == 0 && i + 1 < argc) {
            cfg.rss_limit_bytes = parse_u64(argv[++i], "rss-limit-bytes");
        } else if (strcmp(argv[i], "--cpus") == 0 && i + 1 < argc) {
            cfg.cpus = argv[++i];
        } else if (strcmp(argv[i], "--spin-cpus") == 0 && i + 1 < argc) {
            cfg.spin_cpus_arg = argv[++i];
        } else if (strcmp(argv[i], "--app-cpus") == 0 && i + 1 < argc) {
            cfg.app_cpus_arg = argv[++i];
        } else if (strcmp(argv[i], "--switch-bin") == 0 && i + 1 < argc) {
            cfg.switch_bin = argv[++i];
        } else if (strcmp(argv[i], "--get-percent") == 0 && i + 1 < argc) {
            cfg.get_percent = (int)parse_u64(argv[++i], "get-percent");
        } else if (strcmp(argv[i], "--post-percent") == 0 && i + 1 < argc) {
            cfg.post_percent = (int)parse_u64(argv[++i], "post-percent");
        } else if (strcmp(argv[i], "--ws-percent") == 0 && i + 1 < argc) {
            cfg.ws_percent = (int)parse_u64(argv[++i], "ws-percent");
        } else if (strcmp(argv[i], "--get-hit-percent") == 0 && i + 1 < argc) {
            cfg.get_hit_percent = (int)parse_u64(argv[++i], "get-hit-percent");
        } else {
            fprintf(stderr, "unknown or incomplete argument: %s\n", argv[i]);
            exit(2);
        }
    }
    if (cfg.duration_s == 0U) {
        cfg.duration_s = DEFAULT_DURATION_S;
    }
    if (cfg.inflight == 0U) {
        cfg.inflight = 1U;
    }
    if (cfg.payload_bytes < sizeof(bench_request)) {
        cfg.payload_bytes = sizeof(bench_request);
    }
    if (cfg.get_percent + cfg.post_percent + cfg.ws_percent <= 0) {
        cfg.get_percent = 80;
        cfg.post_percent = 5;
        cfg.ws_percent = 15;
    }
    if (cfg.get_hit_percent < 0) {
        cfg.get_hit_percent = 0;
    } else if (cfg.get_hit_percent > 100) {
        cfg.get_hit_percent = 100;
    }
    return cfg;
}

static void append_cpu_label(char *buffer, size_t cap, int cpu) {
    size_t len = strlen(buffer);
    if (len >= cap) {
        return;
    }
    snprintf(buffer + len, cap - len, "%s%d", len == 0U ? "" : ",", cpu);
}

static void cpu_plan_add(cpu_plan *plan, int cpu) {
    if (plan->usable_cpus >= MAX_BENCH_CPUS || cpu < 0) {
        return;
    }
    plan->usable_cpu_ids[plan->usable_cpus++] = cpu;
    append_cpu_label(plan->usable_cpu_set, sizeof(plan->usable_cpu_set), cpu);
}

static void cpu_plan_add_to_list(int *ids, uint32_t *count, char *label, size_t label_cap, int cpu) {
    if (*count >= MAX_BENCH_CPUS || cpu < 0) {
        return;
    }
    ids[*count] = cpu;
    (*count)++;
    append_cpu_label(label, label_cap, cpu);
}

static void parse_cpu_list_into(int *ids, uint32_t *count, char *label, size_t label_cap, const char *raw) {
    const char *p = raw;
    while (p && *p) {
        char *end = NULL;
        long start = strtol(p, &end, 10);
        if (end == p || start < 0) {
            die_msg("invalid --cpus value");
        }
        long finish = start;
        if (*end == '-') {
            p = end + 1;
            finish = strtol(p, &end, 10);
            if (end == p || finish < start) {
                die_msg("invalid --cpus range");
            }
        }
        for (long cpu = start; cpu <= finish; cpu++) {
            cpu_plan_add_to_list(ids, count, label, label_cap, (int)cpu);
        }
        if (*end == ',') {
            p = end + 1;
        } else {
            p = end;
        }
    }
}

static cpu_plan compute_cpu_plan(const bench_config *cfg) {
    cpu_plan plan;
    memset(&plan, 0, sizeof(plan));
    plan.online_cpus = sysconf(_SC_NPROCESSORS_ONLN);
    if (plan.online_cpus < 1) {
        plan.online_cpus = 1;
    }
    if ((cfg->spin_cpus_arg && *cfg->spin_cpus_arg) || (cfg->app_cpus_arg && *cfg->app_cpus_arg)) {
        if (!cfg->spin_cpus_arg || !*cfg->spin_cpus_arg || !cfg->app_cpus_arg || !*cfg->app_cpus_arg) {
            die_msg("--spin-cpus and --app-cpus must be provided together");
        }
        parse_cpu_list_into(
            plan.spin_cpu_ids,
            &plan.spin_cpus,
            plan.spin_cpu_set,
            sizeof(plan.spin_cpu_set),
            cfg->spin_cpus_arg
        );
        parse_cpu_list_into(
            plan.app_cpu_ids,
            &plan.app_cpus,
            plan.app_cpu_set,
            sizeof(plan.app_cpu_set),
            cfg->app_cpus_arg
        );
        if (plan.app_cpus == 0U) {
            die_msg("--app-cpus must include at least one CPU");
        }
        for (uint32_t i = 0; i < plan.spin_cpus; i++) {
            cpu_plan_add(&plan, plan.spin_cpu_ids[i]);
        }
        for (uint32_t i = 0; i < plan.app_cpus; i++) {
            cpu_plan_add(&plan, plan.app_cpu_ids[i]);
        }
        plan.lane_count = plan.usable_cpus == 0U ? 1U : plan.usable_cpus;
        return plan;
    }
    if (!cfg->cpus || strcmp(cfg->cpus, "auto") == 0) {
        long usable = plan.online_cpus - 2L;
        if (usable < 1L) {
            usable = 1L;
        }
        if (usable > (long)MAX_BENCH_CPUS) {
            usable = (long)MAX_BENCH_CPUS;
        }
        for (long cpu = 0; cpu < usable; cpu++) {
            cpu_plan_add(&plan, (int)cpu);
        }
    } else {
        parse_cpu_list_into(
            plan.usable_cpu_ids,
            &plan.usable_cpus,
            plan.usable_cpu_set,
            sizeof(plan.usable_cpu_set),
            cfg->cpus
        );
        if (plan.usable_cpus == 0U) {
            die_msg("empty --cpus value");
        }
    }

    plan.lane_count = plan.usable_cpus == 0U ? 1U : plan.usable_cpus;
    plan.spin_cpus = (plan.usable_cpus + 3U) / 4U;
    if (plan.usable_cpus <= 1U) {
        plan.spin_cpus = 0U;
    } else if (plan.spin_cpus >= plan.usable_cpus) {
        plan.spin_cpus = plan.usable_cpus - 1U;
    }
    plan.app_cpus = plan.usable_cpus - plan.spin_cpus;
    if (plan.app_cpus == 0U) {
        plan.app_cpus = 1U;
    }

    for (uint32_t i = 0; i < plan.spin_cpus; i++) {
        plan.spin_cpu_ids[i] = plan.usable_cpu_ids[i];
        append_cpu_label(plan.spin_cpu_set, sizeof(plan.spin_cpu_set), plan.spin_cpu_ids[i]);
    }
    for (uint32_t i = 0; i < plan.app_cpus; i++) {
        uint32_t src = plan.spin_cpus + i;
        if (src >= plan.usable_cpus) {
            src = plan.usable_cpus - 1U;
        }
        plan.app_cpu_ids[i] = plan.usable_cpu_ids[src];
        append_cpu_label(plan.app_cpu_set, sizeof(plan.app_cpu_set), plan.app_cpu_ids[i]);
    }
    return plan;
}

static void set_cpu_affinity_ids(const int *cpu_ids, uint32_t count) {
#ifdef __linux__
    if (!cpu_ids || count == 0U) {
        return;
    }
    cpu_set_t set;
    CPU_ZERO(&set);
    for (uint32_t i = 0; i < count; i++) {
        if (cpu_ids[i] >= 0 && cpu_ids[i] < CPU_SETSIZE) {
            CPU_SET(cpu_ids[i], &set);
        }
    }
    (void)sched_setaffinity(0, sizeof(set), &set);
#else
    (void)cpu_ids;
    (void)count;
#endif
}

static void set_thread_cpu_affinity(int cpu) {
#ifdef __linux__
    if (cpu < 0 || cpu >= CPU_SETSIZE) {
        return;
    }
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    (void)pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
#else
    (void)cpu;
#endif
}

static void lane_shm_name(char *buffer, size_t cap, uint32_t channel, uint32_t lane) {
    snprintf(buffer, cap, "%s-%u", CHANNELS[channel].shm_base, lane);
}

static void unlink_bench_shms(uint32_t lane_count) {
    for (uint32_t lane = 0; lane < lane_count; lane++) {
        for (uint32_t i = 0; i < CHANNEL_COUNT; i++) {
            char shm_name[128];
            lane_shm_name(shm_name, sizeof(shm_name), i, lane);
            shm_unlink(shm_name);
        }
    }
}

static pid_t start_switch(const bench_config *cfg, const char *shm_name, const cpu_plan *plan, uint32_t switch_index) {
    pid_t pid = fork();
    if (pid < 0) {
        die("fork switch");
    }
    if (pid == 0) {
        if (plan->spin_cpus > 0U) {
            int cpu = plan->spin_cpu_ids[switch_index % plan->spin_cpus];
            set_cpu_affinity_ids(&cpu, 1U);
        } else {
            set_cpu_affinity_ids(plan->app_cpu_ids, plan->app_cpus);
        }
        execl(
            cfg->switch_bin,
            "datarouter-switch",
            "--name",
            shm_name,
            "--allow",
            "core,cms",
            "--ttl-ms",
            "600000",
            (char *)NULL
        );
        die("execl datarouter-switch");
    }
    return pid;
}

static pid_t start_role_with_stdout(const bench_config *cfg, const cpu_plan *plan, const char *role, const char *self, int stdout_fd) {
    pid_t pid = fork();
    if (pid < 0) {
        die("fork role");
    }
    if (pid == 0) {
        set_cpu_affinity_ids(plan->app_cpu_ids, plan->app_cpus);
        if (stdout_fd >= 0) {
            if (dup2(stdout_fd, STDOUT_FILENO) < 0) {
                die("dup2 role stdout");
            }
            close(stdout_fd);
        }
        char duration[32];
        char inflight[32];
        char payload[32];
        char latency[32];
        char timeout[32];
        char rss_limit[32];
        snprintf(duration, sizeof(duration), "%u", cfg->duration_s);
        snprintf(inflight, sizeof(inflight), "%u", cfg->inflight);
        snprintf(payload, sizeof(payload), "%u", cfg->payload_bytes);
        snprintf(latency, sizeof(latency), "%" PRIu64, cfg->latency_us);
        snprintf(timeout, sizeof(timeout), "%u", cfg->timeout_ms);
        snprintf(rss_limit, sizeof(rss_limit), "%" PRIu64, cfg->rss_limit_bytes);
        execl(
            self,
            self,
            "--role",
            role,
            "--duration",
            duration,
            "--inflight",
            inflight,
            "--payload-bytes",
            payload,
            "--latency-us",
            latency,
            "--timeout-ms",
            timeout,
            "--rss-limit-bytes",
            rss_limit,
            "--cpus",
            cfg->cpus,
            "--spin-cpus",
            cfg->spin_cpus_arg,
            "--app-cpus",
            cfg->app_cpus_arg,
            (char *)NULL
        );
        die("execl role");
    }
    return pid;
}

static pid_t start_role(const bench_config *cfg, const cpu_plan *plan, const char *role, const char *self) {
    return start_role_with_stdout(cfg, plan, role, self, -1);
}

static void stop_process(pid_t pid) {
    if (pid <= 0) {
        return;
    }
    kill(pid, SIGTERM);
    sleep_us(100000ULL);
    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
}

static uint64_t rss_bytes(pid_t pid) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/statm", (int)pid);
    FILE *fp = fopen(path, "r");
    if (!fp) {
        return 0ULL;
    }
    unsigned long pages = 0UL;
    unsigned long rss = 0UL;
    if (fscanf(fp, "%lu %lu", &pages, &rss) != 2) {
        fclose(fp);
        return 0ULL;
    }
    fclose(fp);
    return (uint64_t)rss * (uint64_t)sysconf(_SC_PAGESIZE);
}

static uint64_t combined_rss(pid_t *pids, size_t count) {
    uint64_t total = 0ULL;
    for (size_t i = 0; i < count; i++) {
        total += rss_bytes(pids[i]);
    }
    return total;
}

static ssize_t read_all_fd(int fd, char *buffer, size_t capacity) {
    size_t used = 0U;
    while (used + 1U < capacity) {
        ssize_t n = read(fd, buffer + used, capacity - used - 1U);
        if (n > 0) {
            used += (size_t)n;
            continue;
        }
        if (n == 0) {
            break;
        }
        if (errno == EINTR) {
            continue;
        }
        return -1;
    }
    buffer[used] = '\0';
    return (ssize_t)used;
}

static void print_augmented_result(const char *cms_json, uint64_t max_rss, const bench_config *cfg, const cpu_plan *plan, uint32_t process_count) {
    size_t len = strlen(cms_json);
    while (len > 0U && (cms_json[len - 1U] == '\n' || cms_json[len - 1U] == '\r')) {
        len--;
    }
    if (len == 0U || cms_json[len - 1U] != '}') {
        printf("%s", cms_json);
        return;
    }
    fwrite(cms_json, 1U, len - 1U, stdout);
    printf(
        ",\"max_rss_bytes\":%" PRIu64 ",\"max_rss_limit_bytes\":%" PRIu64
        ",\"cpu_set\":\"%s\",\"spin_cpu_set\":\"%s\",\"app_cpu_set\":\"%s\","
        "\"processes\":%u,\"switch_processes\":%u,\"role_processes\":2,\"lane_threads_per_role\":%u}\n",
        max_rss,
        cfg->rss_limit_bytes,
        plan->usable_cpu_set,
        plan->spin_cpu_set,
        plan->app_cpu_set,
        process_count,
        plan->lane_count * CHANNEL_COUNT,
        plan->lane_count
    );
}

static datarouter_port *open_and_logon(const char *shm_name, const char *service, uint32_t timeout_ms) {
    datarouter_options opts;
    opts.shm_name = shm_name;
    opts.timeout_ms = timeout_ms;
    datarouter_port *port = NULL;
    int rc = DATAROUTER_ERR_NOT_FOUND;
    for (uint32_t i = 0; i < 300U; i++) {
        rc = datarouter_open(&opts, &port);
        if (rc == DATAROUTER_OK) {
            break;
        }
        sleep_us(10000ULL);
    }
    require_ok(rc, "open port");
    char errbuf[128];
    rc = datarouter_logon(port, service, errbuf, sizeof(errbuf));
    if (rc != DATAROUTER_OK) {
        fprintf(stderr, "logon %s failed: %s (%s)\n", service, datarouter_strerror(rc), errbuf);
        exit(1);
    }
    return port;
}

typedef struct {
    const bench_config *cfg;
    const cpu_plan *plan;
    uint32_t lane;
    int cpu;
} core_lane_args;

typedef struct {
    uint64_t sent;
    uint64_t completed;
    uint64_t send_errors;
    uint64_t recv_errors;
    uint64_t channel_sent[CHANNEL_COUNT];
    uint64_t channel_send_errors[CHANNEL_COUNT];
    sample_store samples;
    channel_stats channels[CHANNEL_COUNT];
} lane_result;

typedef struct {
    const bench_config *cfg;
    const cpu_plan *plan;
    uint32_t lane;
    uint32_t per_lane_inflight;
    int cpu;
    lane_result result;
} cms_lane_args;

static void *run_core_lane(void *raw);
static void *run_cms_lane(void *raw);

static int run_core(const bench_config *cfg) {
    cpu_plan plan = compute_cpu_plan(cfg);
    pthread_t *threads = calloc(plan.lane_count, sizeof(*threads));
    core_lane_args *args = calloc(plan.lane_count, sizeof(*args));
    if (!threads || !args) {
        die_msg("core thread allocation failed");
    }
    for (uint32_t lane = 0; lane < plan.lane_count; lane++) {
        args[lane] = (core_lane_args){
            .cfg = cfg,
            .plan = &plan,
            .lane = lane,
            .cpu = plan.app_cpu_ids[lane % plan.app_cpus],
        };
        if (pthread_create(&threads[lane], NULL, (void *(*)(void *))run_core_lane, &args[lane]) != 0) {
            die_msg("core thread create failed");
        }
    }
    for (uint32_t lane = 0; lane < plan.lane_count; lane++) {
        pthread_join(threads[lane], NULL);
    }
    return 0;
}

static void *run_core_lane(void *raw) {
    core_lane_args *args = (core_lane_args *)raw;
    const bench_config *cfg = args->cfg;
    set_thread_cpu_affinity(args->cpu);
    datarouter_port *ports[CHANNEL_COUNT];
    for (uint32_t i = 0; i < CHANNEL_COUNT; i++) {
        char shm_name[128];
        lane_shm_name(shm_name, sizeof(shm_name), i, args->lane);
        ports[i] = open_and_logon(shm_name, "core", cfg->timeout_ms);
    }
    uint8_t *buffer = calloc(1U, cfg->payload_bytes + 64U);
    uint8_t *response = calloc(1U, cfg->payload_bytes + 64U);
    if (!buffer || !response) {
        die_msg("allocation failed");
    }
    for (;;) {
        uint32_t handled = 0U;
        for (uint32_t i = 0; i < CHANNEL_COUNT; i++) {
            datarouter_message_info info;
            int rc = datarouter_recv(ports[i], buffer, cfg->payload_bytes + 64U, &info);
            if (rc == DATAROUTER_ERR_AGAIN) {
                continue;
            }
            if (rc != DATAROUTER_OK) {
                if (rc == DATAROUTER_ERR_NOT_FOUND) {
                    return (void *)(intptr_t)2;
                }
                continue;
            }
            if (info.len < sizeof(bench_request)) {
                continue;
            }
            bench_request req;
            memcpy(&req, buffer, sizeof(req));
            bench_response resp;
            memset(&resp, 0, sizeof(resp));
            resp.seq = req.seq;
            resp.kind = req.kind;
            memcpy(response, &resp, sizeof(resp));
            int send_rc = datarouter_send(ports[i], "cms", response, cfg->payload_bytes);
            if (send_rc != DATAROUTER_OK && send_rc != DATAROUTER_ERR_FULL && send_rc != DATAROUTER_ERR_AGAIN) {
                fprintf(stderr, "core send failed channel=%s: %s\n", CHANNELS[i].name, datarouter_strerror(send_rc));
            }
            handled++;
        }
        if (handled == 0U) {
            sleep_us(50ULL);
        }
    }
    return NULL;
}

static void sample_store_add(sample_store *store, latency_sample sample) {
    if (store->len == store->cap) {
        size_t next = store->cap == 0U ? 4096U : store->cap * 2U;
        latency_sample *items = realloc(store->items, next * sizeof(*items));
        if (!items) {
            die_msg("sample allocation failed");
        }
        store->items = items;
        store->cap = next;
    }
    store->items[store->len++] = sample;
}

static int cmp_u64(const void *a, const void *b) {
    uint64_t av = *(const uint64_t *)a;
    uint64_t bv = *(const uint64_t *)b;
    return (av > bv) - (av < bv);
}

static uint64_t percentile_sorted(const uint64_t *values, size_t count, double pct) {
    if (count == 0U) {
        return 0ULL;
    }
    size_t idx = (size_t)((double)(count - 1U) * pct);
    return values[idx];
}

static void print_summary_from_values(const uint64_t *source, size_t count) {
    if (count == 0U) {
        printf("{\"count\":0}");
        return;
    }
    uint64_t *values = malloc(count * sizeof(*values));
    if (!values) {
        die_msg("summary allocation failed");
    }
    memcpy(values, source, count * sizeof(*values));
    qsort(values, count, sizeof(*values), cmp_u64);
    printf(
        "{\"count\":%zu,\"p50_us\":%" PRIu64 ",\"p95_us\":%" PRIu64 ",\"p99_us\":%" PRIu64 ",\"max_us\":%" PRIu64 "}",
        count,
        percentile_sorted(values, count, 0.50),
        percentile_sorted(values, count, 0.95),
        percentile_sorted(values, count, 0.99),
        values[count - 1U]
    );
    free(values);
}

static void print_sample_summary(const sample_store *store, int selector) {
    uint64_t *values = malloc((store->len == 0U ? 1U : store->len) * sizeof(*values));
    if (!values) {
        die_msg("summary allocation failed");
    }
    size_t count = 0U;
    for (size_t i = 0; i < store->len; i++) {
        const latency_sample *item = &store->items[i];
        int include = 0;
        switch (selector) {
            case 0:
                include = 1;
                break;
            case 1:
                include = item->ipc_us > 0ULL;
                break;
            case 2:
                include = item->kind == KIND_GET;
                break;
            case 3:
                include = item->kind == KIND_GET && item->cache_hit;
                break;
            case 4:
                include = item->kind == KIND_GET && !item->cache_hit;
                break;
            case 5:
                include = item->kind == KIND_POST;
                break;
            case 6:
                include = item->kind == KIND_WS;
                break;
            default:
                include = 0;
        }
        if (include) {
            values[count++] = selector == 1 ? item->ipc_us : item->latency_us;
        }
    }
    print_summary_from_values(values, count);
    free(values);
}

static void record_completion(
    sample_store *samples,
    completion_event *events,
    size_t *event_count,
    size_t idx,
    uint64_t *completed
) {
    completion_event event = events[idx];
    sample_store_add(samples, (latency_sample){
        .kind = event.kind,
        .cache_hit = event.cache_hit,
        .latency_us = event.latency_us,
        .ipc_us = event.ipc_us,
    });
    (*completed)++;
    events[idx] = events[*event_count - 1U];
    (*event_count)--;
}

static void process_due_completions(
    sample_store *samples,
    completion_event *events,
    size_t *event_count,
    uint64_t *completed
) {
    uint64_t now = now_ns();
    size_t i = 0U;
    while (i < *event_count) {
        if (events[i].due_ns <= now) {
            record_completion(samples, events, event_count, i, completed);
        } else {
            i++;
        }
    }
}

static void schedule_completion(
    completion_event *events,
    size_t *event_count,
    size_t event_cap,
    uint8_t kind,
    uint8_t cache_hit,
    uint8_t channel,
    uint64_t started_ns,
    uint64_t ipc_us,
    const bench_config *cfg
) {
    if (*event_count >= event_cap) {
        die_msg("completion queue full");
    }
    uint64_t now = now_ns();
    uint64_t due = started_ns + (cfg->latency_us * 1000ULL);
    if (due < now) {
        due = now;
    }
    uint64_t elapsed_us = ns_to_us(due - started_ns);
    events[*event_count] = (completion_event){
        .kind = kind,
        .cache_hit = cache_hit,
        .channel = channel,
        .due_ns = due,
        .latency_us = elapsed_us,
        .ipc_us = ipc_us,
    };
    (*event_count)++;
}

static uint8_t channel_for_kind(uint8_t kind) {
    if (kind == KIND_POST) {
        return 1U;
    }
    if (kind == KIND_WS) {
        return 2U;
    }
    return 0U;
}

static uint8_t pick_kind(uint64_t seq, const bench_config *cfg, int *get_seen, uint8_t *cache_hit) {
    int total = cfg->get_percent + cfg->post_percent + cfg->ws_percent;
    int slot = (int)(seq % (uint64_t)total);
    *cache_hit = 0U;
    if (slot < cfg->get_percent) {
        (*get_seen)++;
        *cache_hit = (((*get_seen - 1) % 100) < cfg->get_hit_percent) ? 1U : 0U;
        return KIND_GET;
    }
    if (slot < cfg->get_percent + cfg->post_percent) {
        return KIND_POST;
    }
    return KIND_WS;
}

static void pending_add(pending_request *pending, size_t cap, pending_request req) {
    size_t idx = (size_t)(req.seq % cap);
    for (size_t i = 0; i < cap; i++) {
        size_t pos = (idx + i) % cap;
        if (!pending[pos].active) {
            pending[pos] = req;
            pending[pos].active = 1U;
            return;
        }
    }
    die_msg("pending table full");
}

static int pending_remove(pending_request *pending, size_t cap, uint64_t seq, pending_request *out) {
    size_t idx = (size_t)(seq % cap);
    for (size_t i = 0; i < cap; i++) {
        size_t pos = (idx + i) % cap;
        if (pending[pos].active && pending[pos].seq == seq) {
            *out = pending[pos];
            pending[pos].active = 0U;
            return 1;
        }
    }
    return 0;
}

static int send_request(
    datarouter_port *port,
    uint8_t *payload,
    uint32_t payload_bytes,
    uint64_t seq,
    uint8_t kind
) {
    bench_request req;
    memset(&req, 0, sizeof(req));
    req.seq = seq;
    req.kind = kind;
    memset(payload, 0x78, payload_bytes);
    memcpy(payload, &req, sizeof(req));
    return datarouter_send(port, "core", payload, payload_bytes);
}

static int receive_cms_responses(
    datarouter_port **ports,
    pending_request *pending,
    size_t pending_cap,
    completion_event *events,
    size_t *event_count,
    size_t event_cap,
    uint8_t *buffer,
    const bench_config *cfg,
    channel_stats *channel_stats,
    uint64_t *recv_errors
) {
    int received = 0;
    for (uint32_t i = 0; i < CHANNEL_COUNT; i++) {
        for (;;) {
            datarouter_message_info info;
            int rc = datarouter_recv(ports[i], buffer, cfg->payload_bytes + 64U, &info);
            if (rc == DATAROUTER_ERR_AGAIN) {
                break;
            }
            if (rc != DATAROUTER_OK) {
                (*recv_errors)++;
                break;
            }
            if (info.len < sizeof(bench_response)) {
                (*recv_errors)++;
                continue;
            }
            bench_response resp;
            memcpy(&resp, buffer, sizeof(resp));
            pending_request req;
            if (!pending_remove(pending, pending_cap, resp.seq, &req)) {
                (*recv_errors)++;
                continue;
            }
            uint64_t ipc_us = ns_to_us(now_ns() - req.started_ns);
            if (channel_stats[req.channel].ipc_count < channel_stats[req.channel].ipc_cap) {
                channel_stats[req.channel].ipc_us[channel_stats[req.channel].ipc_count++] = ipc_us;
            }
            schedule_completion(
                events,
                event_count,
                event_cap,
                req.kind,
                req.cache_hit,
                req.channel,
                req.started_ns,
                ipc_us,
                cfg
            );
            received++;
        }
    }
    return received;
}

static void print_channel_aggregate(cms_lane_args *args, uint32_t lane_count, uint32_t channel, double elapsed_s) {
    uint64_t sent = 0ULL;
    uint64_t send_errors = 0ULL;
    size_t ipc_count = 0U;
    for (uint32_t lane = 0; lane < lane_count; lane++) {
        sent += args[lane].result.channel_sent[channel];
        send_errors += args[lane].result.channel_send_errors[channel];
        ipc_count += args[lane].result.channels[channel].ipc_count;
    }
    uint64_t *values = malloc((ipc_count == 0U ? 1U : ipc_count) * sizeof(*values));
    if (!values) {
        die_msg("channel aggregate allocation failed");
    }
    size_t pos = 0U;
    for (uint32_t lane = 0; lane < lane_count; lane++) {
        channel_stats *stats = &args[lane].result.channels[channel];
        memcpy(values + pos, stats->ipc_us, stats->ipc_count * sizeof(*values));
        pos += stats->ipc_count;
    }
    printf("\"%s\":{\"sent\":%" PRIu64 ",\"send_errors\":%" PRIu64 ",\"throughput_s\":%.3f,\"ipc_latency\":",
        CHANNELS[channel].name,
        sent,
        send_errors,
        elapsed_s > 0.0 ? (double)sent / elapsed_s : 0.0
    );
    print_summary_from_values(values, ipc_count);
    printf("}");
    free(values);
}

static int run_cms(const bench_config *cfg) {
    cpu_plan plan = compute_cpu_plan(cfg);
    uint32_t per_lane_inflight = (cfg->inflight + plan.lane_count - 1U) / plan.lane_count;
    if (per_lane_inflight == 0U) {
        per_lane_inflight = 1U;
    }
    uint32_t effective_inflight = per_lane_inflight * plan.lane_count;

    pthread_t *threads = calloc(plan.lane_count, sizeof(*threads));
    cms_lane_args *args = calloc(plan.lane_count, sizeof(*args));
    if (!threads || !args) {
        die_msg("cms thread allocation failed");
    }
    uint64_t started_ns = now_ns();
    for (uint32_t lane = 0; lane < plan.lane_count; lane++) {
        args[lane] = (cms_lane_args){
            .cfg = cfg,
            .plan = &plan,
            .lane = lane,
            .per_lane_inflight = per_lane_inflight,
            .cpu = plan.app_cpu_ids[lane % plan.app_cpus],
            .result = {0},
        };
        if (pthread_create(&threads[lane], NULL, run_cms_lane, &args[lane]) != 0) {
            die_msg("cms thread create failed");
        }
    }
    int failed = 0;
    for (uint32_t lane = 0; lane < plan.lane_count; lane++) {
        void *status = NULL;
        pthread_join(threads[lane], &status);
        if ((intptr_t)status != 0) {
            failed = 1;
        }
    }

    sample_store samples = {0};
    uint64_t sent = 0ULL;
    uint64_t completed = 0ULL;
    uint64_t send_errors = 0ULL;
    uint64_t recv_errors = 0ULL;
    for (uint32_t lane = 0; lane < plan.lane_count; lane++) {
        lane_result *result = &args[lane].result;
        sent += result->sent;
        completed += result->completed;
        send_errors += result->send_errors;
        recv_errors += result->recv_errors;
        for (size_t i = 0; i < result->samples.len; i++) {
            sample_store_add(&samples, result->samples.items[i]);
        }
    }

    double elapsed_s = (double)(now_ns() - started_ns) / 1000000000.0;
    printf(
        "{\"sent\":%" PRIu64 ",\"completed\":%" PRIu64 ",\"send_errors\":%" PRIu64
        ",\"recv_errors\":%" PRIu64 ",\"elapsed_s\":%.6f,\"throughput_s\":%.3f,"
        "\"channel_size\":%u,\"duration_s\":%u,\"payload_bytes\":%u,\"inflight\":%u,"
        "\"effective_inflight\":%u,\"per_lane_inflight\":%u,"
        "\"requester_latency_us\":%" PRIu64 ",\"online_cpus\":%ld,\"usable_cpus\":%u,"
        "\"spin_cpus\":%u,\"app_cpus\":%u,\"lane_count\":%u,\"scheduler_cpu_count\":%u,"
        "\"traffic\":{\"get_percent\":%d,\"post_percent\":%d,"
        "\"ws_percent\":%d,\"get_hit_percent\":%d},",
        sent,
        completed,
        send_errors,
        recv_errors,
        elapsed_s,
        elapsed_s > 0.0 ? (double)completed / elapsed_s : 0.0,
        DATAROUTER_CHANNEL_SIZE,
        cfg->duration_s,
        cfg->payload_bytes,
        cfg->inflight,
        effective_inflight,
        per_lane_inflight,
        cfg->latency_us,
        plan.online_cpus,
        plan.usable_cpus,
        plan.spin_cpus,
        plan.app_cpus,
        plan.lane_count,
        plan.spin_cpus,
        cfg->get_percent,
        cfg->post_percent,
        cfg->ws_percent,
        cfg->get_hit_percent
    );
    printf("\"latency\":");
    print_sample_summary(&samples, 0);
    printf(",\"ipc_latency\":");
    print_sample_summary(&samples, 1);
    printf(",\"by_kind\":{\"GET\":");
    print_sample_summary(&samples, 2);
    printf(",\"GET_hit\":");
    print_sample_summary(&samples, 3);
    printf(",\"GET_miss\":");
    print_sample_summary(&samples, 4);
    printf(",\"POST\":");
    print_sample_summary(&samples, 5);
    printf(",\"ws\":");
    print_sample_summary(&samples, 6);
    printf("},\"by_channel\":{");
    for (uint32_t i = 0; i < CHANNEL_COUNT; i++) {
        if (i > 0U) {
            printf(",");
        }
        print_channel_aggregate(args, plan.lane_count, i, elapsed_s);
    }
    printf("},\"by_lane\":{");
    for (uint32_t lane = 0; lane < plan.lane_count; lane++) {
        lane_result *result = &args[lane].result;
        if (lane > 0U) {
            printf(",");
        }
        printf("\"%u\":{\"sent\":%" PRIu64 ",\"completed\":%" PRIu64 ",\"send_errors\":%" PRIu64
            ",\"recv_errors\":%" PRIu64 ",\"throughput_s\":%.3f}",
            lane,
            result->sent,
            result->completed,
            result->send_errors,
            result->recv_errors,
            elapsed_s > 0.0 ? (double)result->completed / elapsed_s : 0.0
        );
    }
    printf("}}\n");

    return !failed && completed == sent && recv_errors == 0ULL ? 0 : 1;
}

static void *run_cms_lane(void *raw) {
    cms_lane_args *args = (cms_lane_args *)raw;
    const bench_config *cfg = args->cfg;
    lane_result *result = &args->result;
    set_thread_cpu_affinity(args->cpu);
    datarouter_port *ports[CHANNEL_COUNT];
    for (uint32_t i = 0; i < CHANNEL_COUNT; i++) {
        char shm_name[128];
        lane_shm_name(shm_name, sizeof(shm_name), i, args->lane);
        ports[i] = open_and_logon(shm_name, "cms", cfg->timeout_ms);
    }

    size_t pending_cap = (size_t)args->per_lane_inflight * 8U + 1024U;
    size_t event_cap = (size_t)args->per_lane_inflight * 4U + 4096U;
    pending_request *pending = calloc(pending_cap, sizeof(*pending));
    completion_event *events = calloc(event_cap, sizeof(*events));
    uint8_t *payload = calloc(1U, cfg->payload_bytes);
    uint8_t *buffer = calloc(1U, cfg->payload_bytes + 64U);
    for (uint32_t i = 0; i < CHANNEL_COUNT; i++) {
        result->channels[i].ipc_cap = (size_t)cfg->duration_s * (size_t)args->per_lane_inflight * 16U + 4096U;
        result->channels[i].ipc_us = calloc(result->channels[i].ipc_cap, sizeof(uint64_t));
        if (!result->channels[i].ipc_us) {
            die_msg("channel sample allocation failed");
        }
    }
    if (!pending || !events || !payload || !buffer) {
        die_msg("allocation failed");
    }

    uint64_t started_ns = now_ns();
    uint64_t stop_ns = started_ns + ((uint64_t)cfg->duration_s * 1000000000ULL);
    uint64_t seq = args->lane;
    int get_seen = 0;
    size_t event_count = 0U;

    while (now_ns() < stop_ns) {
        process_due_completions(&result->samples, events, &event_count, &result->completed);
        (void)receive_cms_responses(
            ports,
            pending,
            pending_cap,
            events,
            &event_count,
            event_cap,
            buffer,
            cfg,
            result->channels,
            &result->recv_errors
        );
        if (result->sent - result->completed >= args->per_lane_inflight) {
            sleep_us(50ULL);
            continue;
        }

        seq += args->plan->lane_count;
        uint8_t cache_hit = 0U;
        uint8_t kind = pick_kind(seq, cfg, &get_seen, &cache_hit);
        uint64_t request_started = now_ns();
        if (kind == KIND_GET && cache_hit) {
            result->sent++;
            schedule_completion(events, &event_count, event_cap, kind, 1U, 0U, request_started, 0ULL, cfg);
            continue;
        }

        uint8_t channel = channel_for_kind(kind);
        pending_add(pending, pending_cap, (pending_request){
            .kind = kind,
            .cache_hit = cache_hit,
            .channel = channel,
            .seq = seq,
            .started_ns = request_started,
        });
        int rc = send_request(ports[channel], payload, cfg->payload_bytes, seq, kind);
        if (rc == DATAROUTER_OK) {
            result->sent++;
            result->channel_sent[channel]++;
        } else {
            pending_request removed;
            (void)pending_remove(pending, pending_cap, seq, &removed);
            result->send_errors++;
            result->channel_send_errors[channel]++;
            if (rc != DATAROUTER_ERR_FULL && rc != DATAROUTER_ERR_AGAIN) {
                fprintf(stderr, "cms send failed: %s\n", datarouter_strerror(rc));
                result->recv_errors++;
            }
            sleep_us(100ULL);
        }
    }

    uint64_t drain_deadline_ns = now_ns() + (DRAIN_DEADLINE_US * 1000ULL);
    while (result->completed < result->sent && now_ns() < drain_deadline_ns) {
        process_due_completions(&result->samples, events, &event_count, &result->completed);
        (void)receive_cms_responses(
            ports,
            pending,
            pending_cap,
            events,
            &event_count,
            event_cap,
            buffer,
            cfg,
            result->channels,
            &result->recv_errors
        );
        sleep_us(100ULL);
    }
    process_due_completions(&result->samples, events, &event_count, &result->completed);
    return (void *)(intptr_t)(result->completed == result->sent && result->recv_errors == 0ULL ? 0 : 1);
}

static int run_coordinator(const bench_config *cfg, const char *self) {
    cpu_plan plan = compute_cpu_plan(cfg);
    uint32_t switch_count = plan.lane_count * CHANNEL_COUNT;
    uint32_t process_count = switch_count + 2U;
    pid_t *switches = calloc(switch_count, sizeof(*switches));
    pid_t *pids = calloc(process_count, sizeof(*pids));
    if (!switches || !pids) {
        die_msg("coordinator allocation failed");
    }
    unlink_bench_shms(plan.lane_count);
    uint32_t switch_index = 0U;
    for (uint32_t lane = 0; lane < plan.lane_count; lane++) {
        for (uint32_t i = 0; i < CHANNEL_COUNT; i++) {
            char shm_name[128];
            lane_shm_name(shm_name, sizeof(shm_name), i, lane);
            switches[switch_index] = start_switch(cfg, shm_name, &plan, switch_index);
            switch_index++;
        }
    }
    sleep_us(150000ULL);
    pid_t core = start_role(cfg, &plan, "core", self);
    sleep_us(150000ULL);
    int cms_pipe[2];
    if (pipe(cms_pipe) != 0) {
        die("pipe cms stdout");
    }
    pid_t cms = start_role_with_stdout(cfg, &plan, "cms", self, cms_pipe[1]);
    close(cms_pipe[1]);

    for (uint32_t i = 0; i < switch_count; i++) {
        pids[i] = switches[i];
    }
    pids[switch_count] = core;
    pids[switch_count + 1U] = cms;

    uint64_t max_rss = 0ULL;
    int cms_status = 0;
    int cms_done = 0;
    int limit_failed = 0;
    while (!cms_done) {
        pid_t done = waitpid(cms, &cms_status, WNOHANG);
        if (done == cms) {
            cms_done = 1;
            break;
        }
        uint64_t rss = combined_rss(pids, process_count);
        if (rss > max_rss) {
            max_rss = rss;
        }
        if (cfg->rss_limit_bytes > 0ULL && rss > cfg->rss_limit_bytes) {
            fprintf(stderr, "combined RSS exceeded limit: rss=%" PRIu64 " limit=%" PRIu64 "\n", rss, cfg->rss_limit_bytes);
            limit_failed = 1;
            stop_process(cms);
            break;
        }
        sleep_us(200000ULL);
    }

    uint64_t final_rss = combined_rss(pids, process_count);
    if (final_rss > max_rss) {
        max_rss = final_rss;
    }

    stop_process(core);
    for (uint32_t i = 0; i < switch_count; i++) {
        stop_process(switches[i]);
    }
    unlink_bench_shms(plan.lane_count);

    char cms_output[65536];
    if (read_all_fd(cms_pipe[0], cms_output, sizeof(cms_output)) >= 0) {
        print_augmented_result(cms_output, max_rss, cfg, &plan, process_count);
    }
    close(cms_pipe[0]);
    if (limit_failed) {
        return 1;
    }
    return WIFEXITED(cms_status) ? WEXITSTATUS(cms_status) : 1;
}

int main(int argc, char **argv) {
    bench_config cfg = parse_args(argc, argv);
    if (strcmp(cfg.role, "core") == 0) {
        return run_core(&cfg);
    }
    if (strcmp(cfg.role, "cms") == 0) {
        return run_cms(&cfg);
    }
    return run_coordinator(&cfg, argv[0]);
}
