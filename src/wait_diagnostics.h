/* CGSGL v2. Compile-time-only observation; never enabled in formal benchmarks. */
#ifndef DATAROUTER_WAIT_DIAGNOSTICS_H
#define DATAROUTER_WAIT_DIAGNOSTICS_H
#ifdef DATAROUTER_WAIT_DIAGNOSTICS
#include <inttypes.h>
/* C endpoint boundaries cover every binding, including runtimes that bypass
 * process destructors. These observations are separate from formal results. */
#include <sys/syscall.h>
extern long syscall(long number, ...);
#define DR_IO_CAPACITY 1048576U
typedef struct {
    uint64_t start, end, cpu_start, cpu_end;
    long tid;
    uint32_t slot, sequence, after, kind;
    int result;
} dr_io_row;
static dr_io_row dr_io_rows[DR_IO_CAPACITY];
static atomic_uint dr_io_count, dr_io_complete[DR_IO_CAPACITY];
static pthread_mutex_t dr_io_mutex = PTHREAD_MUTEX_INITIALIZER;
static uint64_t dr_io_cpu(void) {
    struct timespec ts;
    return clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) == 0
        ? (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec : 0;
}
static uint32_t dr_io_sequence(datarouter_port *port) {
    return port && port->shm && port->slot_index < datarouter_active_max_ports(port->shm)
        ? datarouter_signal_prepare(&port->shm->control.rx_signal[port->slot_index]) : 0U;
}
static dr_io_row dr_io_begin(datarouter_port *port, uint32_t kind) {
    return (dr_io_row){.start=datarouter_now_ns(), .cpu_start=dr_io_cpu(),
        .tid=syscall(SYS_gettid), .slot=port ? port->slot_index : UINT32_MAX,
        .sequence=dr_io_sequence(port), .kind=kind};
}
static void dr_io_end(dr_io_row row, datarouter_port *port, int result) {
    row.end=datarouter_now_ns(); row.cpu_end=dr_io_cpu();
    row.after=dr_io_sequence(port); row.result=result;
    unsigned index=atomic_fetch_add_explicit(&dr_io_count,1U,memory_order_relaxed);
    if (index >= DR_IO_CAPACITY) return;
    dr_io_rows[index]=row;
    atomic_store_explicit(&dr_io_complete[index],1U,memory_order_release);
}
__attribute__((destructor)) static void dr_io_dump(void) {
    const char *base=getenv("DATAROUTER_WAIT_TRACE");
    if (!base || !*base) return;
    char path[4096];
    int len=snprintf(path,sizeof(path),"%s.io.jsonl",base);
    if (len < 0 || (size_t)len >= sizeof(path)) return;
    pthread_mutex_lock(&dr_io_mutex);
    FILE *out=fopen(path,"w");
    if (!out) { pthread_mutex_unlock(&dr_io_mutex); return; }
    unsigned count=atomic_load_explicit(&dr_io_count,memory_order_acquire);
    fprintf(out,"{\"pid\":%ld,\"events\":%u,\"truncated\":%s}\n",(long)getpid(),count,count>DR_IO_CAPACITY ? "true":"false");
    for (unsigned i=0;i<count && i<DR_IO_CAPACITY;++i) {
        if (!atomic_load_explicit(&dr_io_complete[i],memory_order_acquire)) continue;
        dr_io_row *r=&dr_io_rows[i];
        fprintf(out,"{\"kind\":%u,\"tid\":%ld,\"slot\":%u,\"sequence\":%u,\"after\":%u,\"start_ns\":%" PRIu64 ",\"end_ns\":%" PRIu64 ",\"cpu_start_ns\":%" PRIu64 ",\"cpu_end_ns\":%" PRIu64 ",\"result\":%d}\n",r->kind,r->tid,r->slot,r->sequence,r->after,r->start,r->end,r->cpu_start,r->cpu_end,r->result);
    }
    fclose(out); pthread_mutex_unlock(&dr_io_mutex);
}
#define DR_IO_NAME(name) name##_observed_impl
#define DR_WAIT_TRACE_CAPACITY 524288U
typedef struct {
    uint64_t start_ns, end_ns;
    uint32_t polls, empty, busy, spins, kernel, wake_ok, wake_again, wake_timeout;
    int result;
} dr_wait_trace_row;
static dr_wait_trace_row dr_wait_rows[DR_WAIT_TRACE_CAPACITY];
static atomic_uint dr_wait_next;
static atomic_uint dr_wait_complete[DR_WAIT_TRACE_CAPACITY];
static pthread_mutex_t dr_wait_dump_mutex = PTHREAD_MUTEX_INITIALIZER;
static _Thread_local unsigned dr_poll_busy;
static void dr_wait_record(dr_wait_trace_row *row, int result) {
    unsigned index = atomic_fetch_add_explicit(&dr_wait_next, 1U, memory_order_relaxed);
    if (index >= DR_WAIT_TRACE_CAPACITY) return;
    row->end_ns = datarouter_now_ns(); row->result = result;
    dr_wait_rows[index] = *row;
    atomic_store_explicit(&dr_wait_complete[index], 1U, memory_order_release);
}
__attribute__((destructor)) static void dr_wait_dump(void) {
    const char *path = getenv("DATAROUTER_WAIT_TRACE");
    if (!path || !*path) return;
    pthread_mutex_lock(&dr_wait_dump_mutex);
    FILE *out = fopen(path, "w");
    if (!out) { pthread_mutex_unlock(&dr_wait_dump_mutex); return; }
    unsigned count = atomic_load_explicit(&dr_wait_next, memory_order_relaxed);
    fprintf(out, "{\"events\":%u,\"capacity\":%u,\"truncated\":%s}\n", count, DR_WAIT_TRACE_CAPACITY, count > DR_WAIT_TRACE_CAPACITY ? "true" : "false");
    for (unsigned i = 0; i < count && i < DR_WAIT_TRACE_CAPACITY; ++i) {
        dr_wait_trace_row *r = &dr_wait_rows[i];
        if (!atomic_load_explicit(&dr_wait_complete[i], memory_order_acquire)) continue;
        fprintf(out, "{\"index\":%u,\"start_ns\":%" PRIu64 ",\"end_ns\":%" PRIu64 ",\"polls\":%u,\"empty\":%u,\"lock_busy\":%u,\"spins\":%u,\"wait_helper_calls\":%u,\"wake_ok\":%u,\"wake_again\":%u,\"wake_timeout\":%u,\"result\":%d}\n", i, r->start_ns, r->end_ns, r->polls, r->empty, r->busy, r->spins, r->kernel, r->wake_ok, r->wake_again, r->wake_timeout, r->result);
    }
    fclose(out);
    pthread_mutex_unlock(&dr_wait_dump_mutex);
}
#define DR_WAIT_BEGIN(ts) dr_wait_trace_row observation = {.start_ns = (ts)}
#define DR_WAIT_RETURN(code) do { int result_ = (code); dr_wait_record(&observation, result_); return result_; } while (0)
#define DR_WAIT_POLL(code) do { ++observation.polls; observation.busy += dr_poll_busy; observation.empty += ((code) == DATAROUTER_ERR_AGAIN && !dr_poll_busy); } while (0)
#define DR_WAIT_SPIN() (++observation.spins)
#define DR_WAIT_KERNEL(code) do { ++observation.kernel; observation.wake_ok += ((code) == DATAROUTER_OK); observation.wake_again += ((code) == DATAROUTER_ERR_AGAIN); observation.wake_timeout += ((code) == DATAROUTER_ERR_TIMEOUT); } while (0)
#else
#define DR_IO_NAME(name) name
#define DR_WAIT_BEGIN(ts) ((void)0)
#define DR_WAIT_RETURN(code) return (code)
#define DR_WAIT_POLL(code) ((void)0)
#define DR_WAIT_SPIN() ((void)0)
#define DR_WAIT_KERNEL(code) ((void)0)
#endif
#endif
