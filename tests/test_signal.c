#define _GNU_SOURCE
#include "../src/datarouter_signal.h"
#include <assert.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#include <pthread.h>

static void reaped(pid_t pid) {
    int status;
    assert(waitpid(pid, &status, 0) == pid);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

#if DATAROUTER_LAYOUT_VERSION >= 16U
static void wait_for_sleepers(datarouter_signal *s, unsigned int count) {
    uint64_t deadline = datarouter_now_ns() + 1000000000ULL;
    while (atomic_load(&s->waiters) != count) {
        assert(datarouter_now_ns() < deadline);
        usleep(100U);
    }
}
static pid_t sleeper(datarouter_signal *s) {
    pid_t pid = fork();
    assert(pid >= 0);
    if (!pid) {
        uint32_t token = datarouter_signal_prepare(s);
        int rc = datarouter_signal_wait_prepared(s, token, 2000U);
        _exit(rc == DATAROUTER_OK ? 0 : 1);
    }
    return pid;
}
static void interrupted(int signum) { (void)signum; }
#endif

#ifdef DATAROUTER_HAS_SLEEPER_NOTIFICATION
static pthread_barrier_t race_boundary;
static datarouter_signal race_signal;
static atomic_uint race_ready;
static unsigned int race_observed;
static void *race_publish(void *unused) {
    (void)unused;
    for (unsigned i = 0; i < 20000; i++) {
        pthread_barrier_wait(&race_boundary);
        atomic_store_explicit(&race_ready, 1, memory_order_release);
        datarouter_signal_post_if_waiting(&race_signal);
        pthread_barrier_wait(&race_boundary);
    }
    return NULL;
}
static void *race_sleep(void *unused) {
    (void)unused;
    for (unsigned i = 0; i < 20000; i++) {
        pthread_barrier_wait(&race_boundary);
        assert(datarouter_signal_arm(&race_signal) == DATAROUTER_OK);
        race_observed = atomic_load_explicit(&race_ready, memory_order_acquire);
        pthread_barrier_wait(&race_boundary);
    }
    return NULL;
}
static void test_sleeper_publication_race(void) {
    assert(pthread_barrier_init(&race_boundary, NULL, 3) == 0);
    pthread_t publisher, sleeper_thread;
    assert(pthread_create(&publisher, NULL, race_publish, NULL) == 0);
    assert(pthread_create(&sleeper_thread, NULL, race_sleep, NULL) == 0);
    for (unsigned i = 0; i < 20000; i++) {
        datarouter_signal_init(&race_signal); atomic_store(&race_ready, 0);
        pthread_barrier_wait(&race_boundary);
        pthread_barrier_wait(&race_boundary);
        assert(race_observed || datarouter_signal_prepare(&race_signal) != 0);
        datarouter_signal_cancel(&race_signal);
    }
    pthread_join(publisher, NULL); pthread_join(sleeper_thread, NULL);
    pthread_barrier_destroy(&race_boundary);
}
#endif

int main(void) {
    alarm(10);
#ifdef DATAROUTER_HAS_SLEEPER_NOTIFICATION
    test_sleeper_publication_race();
#endif
    datarouter_signal *s = mmap(NULL, sizeof(*s), PROT_READ | PROT_WRITE,
                                MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    assert(s != MAP_FAILED);
    assert(datarouter_signal_init(s) == DATAROUTER_OK);
    assert(datarouter_signal_wait(NULL, 1U) == DATAROUTER_ERR_INVALID_ARGUMENT);
    for (unsigned int i = 0; i < 1000000U; ++i) datarouter_signal_post(s);
    assert(datarouter_signal_wait(s, 1U) == DATAROUTER_OK);
#ifdef DATAROUTER_HAS_SLEEPER_NOTIFICATION
    uint32_t unchanged = datarouter_signal_prepare(s);
    datarouter_signal_post_if_waiting(s);
    assert(datarouter_signal_prepare(s) == unchanged); /* no notification write */
    assert(datarouter_signal_arm(s) == DATAROUTER_OK);
    uint32_t armed = datarouter_signal_prepare(s);
    datarouter_signal_post_if_waiting(s); /* publish after predicate recheck */
    assert(datarouter_signal_wait_armed(s, armed, 1) == DATAROUTER_OK);
    assert(atomic_load(&s->waiters) == 0);
    assert(datarouter_signal_arm(s) == DATAROUTER_OK);
    datarouter_signal_cancel(s); /* nonempty predicate, no kernel wait */
    assert(atomic_load(&s->waiters) == 0);
    /* Consume the hint from the armed case before testing semaphore debt. */
    assert(datarouter_signal_wait(s, 1U) == DATAROUTER_OK);
#endif
    /* A million publications must not leave a million empty wakeups. */
    assert(datarouter_signal_wait(s, 1U) == DATAROUTER_ERR_TIMEOUT);
#if DATAROUTER_LAYOUT_VERSION >= 16U
    uint32_t token = datarouter_signal_prepare(s);
    datarouter_signal_post(s);
    assert(datarouter_signal_wait(s, 1U) == DATAROUTER_OK);
    assert(datarouter_signal_wait_prepared(s, token, 1U) == DATAROUTER_OK);
    /* Unsigned sequence wrap still changes a previously prepared token. */
    atomic_store(&s->sequence, UINT_MAX);
    token = datarouter_signal_prepare(s);
    datarouter_signal_post(s);
    assert(datarouter_signal_wait_prepared(s, token, 1U) == DATAROUTER_OK);
    pid_t first = sleeper(s), second = sleeper(s);
    wait_for_sleepers(s, 2U);
    datarouter_signal_post(s);
    reaped(first); reaped(second);
    assert(atomic_load(&s->waiters) == 0U);

    /* Killed waiters only leave a conservative wake count, never a lock. */
    first = sleeper(s);
    wait_for_sleepers(s, 1U);
    assert(kill(first, SIGSTOP) == 0);
    assert(kill(first, SIGKILL) == 0);
    int status;
    assert(waitpid(first, &status, 0) == first && WIFSIGNALED(status));
    second = sleeper(s);
    wait_for_sleepers(s, 2U);
    datarouter_signal_post(s);
    reaped(second);
    assert(atomic_load(&s->waiters) == 1U);
    atomic_store(&s->waiters, UINT_MAX);
    assert(datarouter_signal_wait_prepared(s, datarouter_signal_prepare(s), 1U) == DATAROUTER_ERR_PLATFORM);
    assert(atomic_load(&s->waiters) == UINT_MAX);
    atomic_store(&s->waiters, 0U);

    struct sigaction action = {0};
    action.sa_handler = interrupted;
    assert(sigaction(SIGUSR1, &action, NULL) == 0);
    pid_t parent = getpid();
    first = fork();
    assert(first >= 0);
    if (!first) {
        for (unsigned int i = 0; i < 40U; ++i) {
            usleep(1000U);
            (void)kill(parent, SIGUSR1);
        }
        _exit(0);
    }
    uint64_t started = datarouter_now_ns();
    assert(datarouter_signal_wait_prepared(s, datarouter_signal_prepare(s), 20U) == DATAROUTER_ERR_TIMEOUT);
    uint64_t elapsed = datarouter_now_ns() - started;
    assert(elapsed >= 19000000ULL && elapsed < 1000000000ULL);
    /* The sender can still interrupt waitpid; reap with EINTR handling. */
    while (waitpid(first, &status, 0) < 0) {}
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
#endif
    assert(munmap(s, sizeof(*s)) == 0);
    puts("signal tests passed");
    return 0;
}
