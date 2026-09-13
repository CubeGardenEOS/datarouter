/* CGSGL v2. Implementation included by common.c so every C binding shares it. */
#ifndef DATAROUTER_THREAD_POLICY_H
#define DATAROUTER_THREAD_POLICY_H

void datarouter_cpu_pause(void) {
#if defined(__i386__) || defined(__x86_64__)
    __asm__ __volatile__("pause" ::: "memory");
#elif defined(__aarch64__)
    __asm__ __volatile__("yield" ::: "memory");
#else
    atomic_signal_fence(memory_order_seq_cst);
#endif
}

#ifndef _WIN32
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/syscall.h>
#include <limits.h>
#include <dirent.h>

typedef struct dr_thread_lease {
    struct dr_thread_lease *next;
    cpu_set_t original;
    int lease_fd;
    int quota_fds[64];
    unsigned int quota_count;
    int mode; /* 1: dedicated, 0: hybrid, -1: explicit spin failed */
    pid_t pid;
} dr_thread_lease;
static pthread_key_t dr_lease_key;
static pthread_once_t dr_lease_once = PTHREAD_ONCE_INIT;
static int dr_lease_key_error;
static pthread_mutex_t dr_lease_mutex = PTHREAD_MUTEX_INITIALIZER;
static dr_thread_lease *dr_leases;
/* pthread key retains destructor ownership; this pointer is only a local shortcut. */
static _Thread_local dr_thread_lease *dr_cached_lease;
static void dr_release_quotas(dr_thread_lease *lease) {
    while (lease->quota_count) close(lease->quota_fds[--lease->quota_count]);
}
static void dr_before_fork(void) { pthread_mutex_lock(&dr_lease_mutex); }
static void dr_after_fork_parent(void) { pthread_mutex_unlock(&dr_lease_mutex); }
static void dr_after_fork_child(void) {
    dr_cached_lease = NULL;
    dr_thread_lease *self = pthread_getspecific(dr_lease_key);
    if (self && self->lease_fd >= 0)
        (void)sched_setaffinity(0, sizeof(self->original), &self->original);
    /* Closing inherited copies must not retain another process's lease if
     * the child does not exec. Parent descriptors remain open and owned. */
    for (dr_thread_lease *p = dr_leases; p; p = p->next) {
        if (p->lease_fd >= 0) close(p->lease_fd);
        p->lease_fd = -1;
        dr_release_quotas(p);
    }
    pthread_mutex_unlock(&dr_lease_mutex);
}

static void dr_lease_destroy(void *value) {
    dr_thread_lease *lease = value;
    if (!lease) return;
    if (dr_cached_lease == lease) dr_cached_lease = NULL;
    pthread_mutex_lock(&dr_lease_mutex);
    dr_thread_lease **link = &dr_leases;
    while (*link && *link != lease) link = &(*link)->next;
    if (*link) *link = lease->next;
    if (lease->lease_fd >= 0) close(lease->lease_fd);
    dr_release_quotas(lease);
    pthread_mutex_unlock(&dr_lease_mutex);
    free(lease);
}
static void dr_lease_initialize(void) {
    dr_lease_key_error = pthread_key_create(&dr_lease_key, dr_lease_destroy);
    if (!dr_lease_key_error)
        dr_lease_key_error = pthread_atfork(dr_before_fork, dr_after_fork_parent, dr_after_fork_child);
}

/* Linux abstract sockets provide an atomic, process-independent lease. The
 * last fd close (including process death) releases it without stale files.
 * Key by physical core, so siblings cannot be leased by different pollers. */
static int dr_named_lease(const char *name) {
    int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    struct sockaddr_un address = {0};
    address.sun_family = AF_UNIX;
    int n = snprintf(address.sun_path + 1, sizeof(address.sun_path) - 1,
                     "%s", name);
    if (n < 0 || (size_t)n >= sizeof(address.sun_path) - 1 ||
        bind(fd, (struct sockaddr *)&address,
             (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + n)) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static int dr_core_lease(int package, int core) {
    char name[80]; snprintf(name, sizeof(name), "datarouter-core-v1-%d-%d", package, core);
    return dr_named_lease(name);
}

static int dr_cpu_topology(int cpu, const char *field) {
    /* Protected by dr_lease_mutex; immutable hardware topology is cached so
     * Go's per-call detach does not reread every sysfs file. */
    static int values[2][CPU_SETSIZE];
    static unsigned char loaded[2][CPU_SETSIZE];
    unsigned int kind = strcmp(field, "core_id") == 0;
    if (loaded[kind][cpu]) return values[kind][cpu];
    char path[160];
    snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/topology/%s", cpu, field);
    FILE *f = fopen(path, "r");
    int value = -1;
    if (f) { if (fscanf(f, "%d", &value) != 1) value = -1; fclose(f); }
    values[kind][cpu] = value; loaded[kind][cpu] = 1;
    return value;
}

static void dr_cpu_domain(int cpu, int *node, int *cache) {
    static unsigned char loaded[CPU_SETSIZE];
    static int nodes[CPU_SETSIZE], caches[CPU_SETSIZE];
    if (!loaded[cpu]) {
        char path[160]; snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d", cpu);
        DIR *directory = opendir(path); nodes[cpu] = -1; caches[cpu] = -1;
        if (directory) {
            struct dirent *entry;
            while ((entry = readdir(directory))) {
                int id; char extra;
                if (sscanf(entry->d_name, "node%d%c", &id, &extra) == 1) nodes[cpu] = id;
            }
            closedir(directory);
        }
        int highest = -1;
        for (int index = 0; index < 16; index++) {
            snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/cache/index%d/level", cpu, index);
            FILE *file = fopen(path, "r"); int level = -1;
            if (!file) continue;
            if (fscanf(file, "%d", &level) != 1) level = -1;
            fclose(file);
            if (level <= highest) continue;
            snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/cache/index%d/id", cpu, index);
            file = fopen(path, "r"); int id = -1;
            if (file) { if (fscanf(file, "%d", &id) != 1) id = -1; fclose(file); }
            if (id >= 0) { highest = level; caches[cpu] = id; }
        }
        loaded[cpu] = 1;
    }
    *node = nodes[cpu]; *cache = caches[cpu];
}

/* Strict CPU list: reject typos instead of silently widening affinity. */
static int dr_cpu_list(const char *text, cpu_set_t *set) {
    CPU_ZERO(set);
    if (!text || !*text) return 0;
    for (;;) {
        if (*text < '0' || *text > '9') return 0;
        char *end;
        long first = strtol(text, &end, 10), last = first;
        if (first < 0 || first >= CPU_SETSIZE) return 0;
        if (*end == '-') {
            text = end + 1;
            if (*text < '0' || *text > '9') return 0;
            last = strtol(text, &end, 10);
            if (last < first || last >= CPU_SETSIZE) return 0;
        }
        for (long cpu = first; cpu <= last; cpu++) CPU_SET(cpu, set);
        if (!*end) return 1;
        if (*end != ',' || !end[1]) return 0;
        text = end + 1;
    }
}

/* Walk v2 ancestors: the effective budget is the smallest quota. A v2 root
 * has no cpu.max and is unlimited. Unknown/v1 mounts fail conservatively. */
static int dr_cpu_quota(dr_thread_lease *lease) {
    FILE *f = fopen("/proc/self/cgroup", "r");
    if (!f) return 0;
    char line[PATH_MAX], group[PATH_MAX] = "";
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "0::/", 4) == 0) {
            snprintf(group, sizeof(group), "%s", line + 3);
            group[strcspn(group, "\r\n")] = 0;
        }
    }
    fclose(f);
    if (!group[0] || strstr(group, "..")) return 0;
    char directory[PATH_MAX];
    if (snprintf(directory, sizeof(directory), "/sys/fs/cgroup%s", group) >= (int)sizeof(directory)) return 0;
    size_t root = strlen("/sys/fs/cgroup");
    int limit = CPU_SETSIZE;
    for (;;) {
        size_t len = strlen(directory);
        if (len > root && directory[len-1] == '/') directory[--len] = 0;
        char path[PATH_MAX + 16];
        snprintf(path, sizeof(path), "%s/cpu.max", directory);
        f = fopen(path, "r");
        if (f) {
            char quota[64]; unsigned long long period;
            int fields = fscanf(f, "%63s %llu", quota, &period);
            fclose(f);
            if (fields != 2 || !period) return 0;
            if (strcmp(quota, "max") != 0) {
                char *end; unsigned long long q = strtoull(quota, &end, 10);
                if (*end) return 0;
                unsigned long long cores = q / period;
                if (cores < (unsigned int)limit) limit = (int)cores;
                if (cores <= 2 || lease->quota_count == 64) return 0;
                struct stat identity;
                if (stat(directory, &identity)) return 0;
                int fd = -1;
                unsigned int slots = cores-2 < CPU_SETSIZE ? (unsigned int)(cores-2) : CPU_SETSIZE;
                for (unsigned int slot = 0; slot < slots && fd < 0; slot++) {
                    char name[100];
                    snprintf(name, sizeof(name), "datarouter-quota-v1-%llu-%llu-%u",
                             (unsigned long long)identity.st_dev, (unsigned long long)identity.st_ino, slot);
                    fd = dr_named_lease(name);
                }
                if (fd < 0) return 0;
                lease->quota_fds[lease->quota_count++] = fd;
            }
        } else if (len != root || errno != ENOENT) return 0;
        if (len == root) break;
        char *slash = strrchr(directory, '/');
        if (!slash || (size_t)(slash-directory) < root) return 0;
        *slash = 0;
    }
    return limit;
}

/* Keep topology/quota scratch storage off the cached fast path stack. */
static __attribute__((noinline)) int dr_thread_attach_slow(void) {
    dr_cached_lease = NULL;
    pthread_once(&dr_lease_once, dr_lease_initialize);
    if (dr_lease_key_error) return -1;
    dr_thread_lease *lease = pthread_getspecific(dr_lease_key);
    if (lease && lease->pid == getpid()) {
        dr_cached_lease = lease;
        return lease->mode;
    }
    if (lease) { /* fork child cannot inherit its parent's lease ownership */
        pthread_setspecific(dr_lease_key, NULL);
        if (sched_setaffinity(0, sizeof(lease->original), &lease->original) != 0) {
            dr_lease_destroy(lease); return -1;
        }
        dr_lease_destroy(lease);
    }
    lease = calloc(1, sizeof(*lease));
    if (!lease) return -1;
    lease->lease_fd = -1;
    lease->pid = getpid();
    if (pthread_setspecific(dr_lease_key, lease) != 0) { free(lease); return -1; }
    pthread_mutex_lock(&dr_lease_mutex);
    lease->next = dr_leases; dr_leases = lease;
    const char *mode = getenv("DATAROUTER_WAIT_MODE");
    int explicit_spin = mode && strcmp(mode, "spin") == 0;
    const char *reason = "insufficient-physical-cores-or-quota";
    lease->mode = -1;
    dr_cached_lease = lease;
    if (mode && strcmp(mode, "auto") && strcmp(mode, "spin") && strcmp(mode, "hybrid")) {
        reason = "invalid-wait-mode"; goto failed;
    }
    if (mode && strcmp(mode, "hybrid") == 0) { lease->mode = 0; pthread_mutex_unlock(&dr_lease_mutex); return 0; }
    if (sched_getaffinity(0, sizeof(lease->original), &lease->original) != 0) {
        reason = "affinity-unavailable"; goto fallback;
    }
    cpu_set_t allowed = lease->original;
    const char *configured = getenv("DATAROUTER_CPUS");
    if (configured) {
        cpu_set_t requested;
        if (!dr_cpu_list(configured, &requested)) { reason = "invalid-cpu-list"; goto failed; }
        CPU_AND(&allowed, &allowed, &requested);
    }
    int cpus[CPU_SETSIZE], packages[CPU_SETSIZE], cores[CPU_SETSIZE], count = 0;
    for (int cpu = 0; cpu < CPU_SETSIZE; cpu++) {
        if (!CPU_ISSET(cpu, &allowed)) continue;
        int package = dr_cpu_topology(cpu, "physical_package_id");
        int core = dr_cpu_topology(cpu, "core_id");
        if (package < 0 || core < 0) continue;
        int duplicate = 0;
        for (int i = 0; i < count; i++) if (packages[i] == package && cores[i] == core) duplicate = 1;
        if (!duplicate) { cpus[count] = cpu; packages[count] = package; cores[count++] = core; }
    }
    if (count) {
        int node, cache; dr_cpu_domain(cpus[0], &node, &cache);
        int rank[CPU_SETSIZE];
        for (int i = 0; i < count; i++) {
            int other_node, other_cache; dr_cpu_domain(cpus[i], &other_node, &other_cache);
            rank[i] = (node >= 0 && node == other_node ? 2 : 0) +
                      (cache >= 0 && cache == other_cache ? 1 : 0);
        }
        for (int i = 0; i < count; i++) for (int j = i+1; j < count; j++) {
            if (rank[j] <= rank[i]) continue;
            int value = rank[i]; rank[i] = rank[j]; rank[j] = value;
            value = cpus[i]; cpus[i] = cpus[j]; cpus[j] = value;
            value = packages[i]; packages[i] = packages[j]; packages[j] = value;
            value = cores[i]; cores[i] = cores[j]; cores[j] = value;
        }
    }
    int quota = dr_cpu_quota(lease);
    /* Automatic allocation reserves two physical cores. Explicit spin with
     * an explicit CPU pool is externally allocated (e.g. frozen benchmark
     * roles); the coordinator reserves cores outside that pool. Quota still
     * reserves two CPU-equivalents, including for explicit allocation. */
    int budget = explicit_spin && configured ? count : count - 2;
    if (budget > quota - 2) budget = quota - 2;
    for (int i = 0; i < budget; i++) {
        int fd = dr_core_lease(packages[i], cores[i]);
        if (fd < 0) continue;
        cpu_set_t single; CPU_ZERO(&single); CPU_SET(cpus[i], &single);
        if (sched_setaffinity(0, sizeof(single), &single) != 0) { close(fd); continue; }
        lease->lease_fd = fd; lease->mode = 1;
        pthread_mutex_unlock(&dr_lease_mutex);
        fprintf(stderr, "datarouter wait=spin pid=%d tid=%ld cpu=%d package=%d core=%d lease=held\n",
                (int)getpid(), syscall(SYS_gettid), cpus[i], packages[i], cores[i]);
        return 1;
    }
fallback:
    if (!explicit_spin) lease->mode = 0;
failed:
    dr_release_quotas(lease);
    pthread_mutex_unlock(&dr_lease_mutex);
    fprintf(stderr, "datarouter wait=%s pid=%d tid=%ld reason=%s\n",
            lease->mode < 0 ? "error" : "hybrid", (int)getpid(), syscall(SYS_gettid), reason);
    return lease->mode;
}

int datarouter_thread_attach(void) {
    dr_thread_lease *cached = dr_cached_lease;
    if (cached && cached->pid == getpid()) return cached->mode;
    return dr_thread_attach_slow();
}

void datarouter_thread_detach(void) {
    pthread_once(&dr_lease_once, dr_lease_initialize);
    if (dr_lease_key_error) return;
    dr_thread_lease *lease = pthread_getspecific(dr_lease_key);
    if (!lease) return;
    if (lease->lease_fd >= 0 && sched_setaffinity(0, sizeof(lease->original), &lease->original) != 0) {
        fprintf(stderr, "datarouter affinity restore failed pid=%d tid=%ld errno=%d\n",
                (int)getpid(), syscall(SYS_gettid), errno);
    }
    pthread_setspecific(dr_lease_key, NULL);
    dr_lease_destroy(lease);
}
#else
int datarouter_thread_attach(void) {
    const char *mode = getenv("DATAROUTER_WAIT_MODE");
    return mode && strcmp(mode, "auto") && strcmp(mode, "hybrid") ? -1 : 0;
}
void datarouter_thread_detach(void) {}
#endif
#endif
