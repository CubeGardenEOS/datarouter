/* CGSGL v2. Independent diagnostic builds only. Raw syscall observations do
 * not establish how much wall time was spent blocked versus runnable. */
#ifndef DATAROUTER_SIGNAL_DIAGNOSTICS_H
#define DATAROUTER_SIGNAL_DIAGNOSTICS_H
#if defined(DATAROUTER_WAIT_DIAGNOSTICS) && !defined(_WIN32)
#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>
#include <pthread.h>
#define DR_SIGNAL_CAPACITY 1048576U
typedef struct {
    uint64_t start, end, cpu_start, cpu_end;
    uintptr_t address;
    long tid, result;
    int error;
    uint32_t sequence, after, kind;
} dr_signal_event;
static dr_signal_event dr_signal_events[DR_SIGNAL_CAPACITY];
static atomic_uint dr_signal_count;
static pthread_mutex_t dr_signal_dump_mutex=PTHREAD_MUTEX_INITIALIZER;
static atomic_uint dr_signal_complete[DR_SIGNAL_CAPACITY];
static uint64_t dr_signal_cpu(void) {
    struct timespec ts;
    return clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) == 0
        ? (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec : 0;
}
static dr_signal_event dr_signal_begin(datarouter_signal *signal, uint32_t sequence, uint32_t kind) {
    return (dr_signal_event){.start=datarouter_now_ns(), .cpu_start=dr_signal_cpu(),
        .address=(uintptr_t)signal, .tid=syscall(SYS_gettid), .sequence=sequence, .kind=kind};
}
static void dr_signal_end(dr_signal_event event, long result, int error, datarouter_signal *signal) {
    event.cpu_end=dr_signal_cpu(); event.end=datarouter_now_ns();
    event.result=result; event.error=error;
    event.after=atomic_load_explicit(&signal->sequence, memory_order_seq_cst);
    unsigned i=atomic_fetch_add_explicit(&dr_signal_count,1U,memory_order_relaxed);
    if (i>=DR_SIGNAL_CAPACITY) return;
    dr_signal_events[i]=event;
    atomic_store_explicit(&dr_signal_complete[i],1U,memory_order_release);
}
__attribute__((destructor)) static void dr_signal_dump(void) {
    const char *base=getenv("DATAROUTER_WAIT_TRACE");
    if (!base || !*base) return;
    char path[4096];
    if (snprintf(path,sizeof(path),"%s.signal.%ld.jsonl",base,(long)getpid()) >= (int)sizeof(path)) return;
    pthread_mutex_lock(&dr_signal_dump_mutex);
    FILE *out=fopen(path,"w");
    if (!out) { pthread_mutex_unlock(&dr_signal_dump_mutex); return; }
    unsigned count=atomic_load_explicit(&dr_signal_count,memory_order_acquire);
    fprintf(out,"{\"pid\":%ld,\"events\":%u,\"capacity\":%u,\"truncated\":%s,\"kinds\":{\"1\":\"FUTEX_WAIT\",\"2\":\"FUTEX_WAKE\",\"3\":\"sequence_changed\",\"4\":\"switch_spin_armed\",\"5\":\"switch_maintenance_wait\"}}\n",(long)getpid(),count,DR_SIGNAL_CAPACITY,count>DR_SIGNAL_CAPACITY?"true":"false");
    for(unsigned i=0;i<count && i<DR_SIGNAL_CAPACITY;i++) {
        if(!atomic_load_explicit(&dr_signal_complete[i],memory_order_acquire)) continue;
        dr_signal_event *r=&dr_signal_events[i];
        fprintf(out,"{\"tid\":%ld,\"kind\":%u,\"address\":%" PRIuPTR ",\"sequence\":%u,\"after\":%u,\"start_ns\":%" PRIu64 ",\"end_ns\":%" PRIu64 ",\"cpu_start_ns\":%" PRIu64 ",\"cpu_end_ns\":%" PRIu64 ",\"return\":%ld,\"errno\":%d}\n",r->tid,r->kind,r->address,r->sequence,r->after,r->start,r->end,r->cpu_start,r->cpu_end,r->result,r->error);
    }
    fclose(out);
    pthread_mutex_unlock(&dr_signal_dump_mutex);
    /* Capture the address->shared-memory inode/offset mapping before unmap
     * is not guaranteed at exit; callers must also capture /proc/PID/maps
     * during sampling. An absent mapping remains uncorrelated evidence. */
}
#define DR_SIGNAL_BEGIN(s,q,k) dr_signal_event event=dr_signal_begin(s,q,k)
#define DR_SIGNAL_END(r,e,s) dr_signal_end(event,r,e,s)
#else
#define DR_SIGNAL_BEGIN(s,q,k) ((void)0)
#define DR_SIGNAL_END(r,e,s) ((void)0)
#endif
#endif
