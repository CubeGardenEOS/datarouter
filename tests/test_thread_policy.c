#define _GNU_SOURCE
#include "../src/datarouter_internal.h"
#include <assert.h>
#include <sched.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>
#include <signal.h>

static cpu_set_t original;
static cpu_set_t first;
static void *claim_and_exit(void *unused) {
    (void)unused;
    assert(datarouter_thread_attach() == 1);
    assert(sched_getaffinity(0, sizeof(first), &first) == 0);
    assert(CPU_COUNT(&first) == 1);
    return NULL; /* pthread destructor must return the core lease */
}

int main(void) {
    assert(sched_getaffinity(0, sizeof(original), &original) == 0);
    assert(setenv("DATAROUTER_WAIT_MODE", "hybrid", 1) == 0);
    assert(datarouter_thread_attach() == 0);
    datarouter_thread_detach();
    assert(setenv("DATAROUTER_WAIT_MODE", "invalid", 1) == 0);
    assert(datarouter_thread_attach() == -1);
    datarouter_thread_detach();
    assert(setenv("DATAROUTER_WAIT_MODE", "spin", 1) == 0);
    assert(setenv("DATAROUTER_CPUS", "0,,1", 1) == 0);
    assert(datarouter_thread_attach() == -1);
    datarouter_thread_detach();
    int one = -1;
    for (int i = 0; i < CPU_SETSIZE; i++) if (CPU_ISSET(i, &original)) { one = i; break; }
    assert(one >= 0);
    char list[32]; snprintf(list, sizeof(list), "%d", one);
    assert(setenv("DATAROUTER_CPUS", list, 1) == 0);
    cpu_set_t single; CPU_ZERO(&single); CPU_SET(one, &single);
    assert(sched_setaffinity(0, sizeof(single), &single) == 0);
    unsetenv("DATAROUTER_CPUS");
    assert(datarouter_thread_attach() == -1); /* automatic allocation needs reserves */
    datarouter_thread_detach();
    assert(setenv("DATAROUTER_WAIT_MODE", "auto", 1) == 0);
    assert(datarouter_thread_attach() == 0);
    datarouter_thread_detach();
    assert(sched_setaffinity(0, sizeof(original), &original) == 0);
    unsetenv("DATAROUTER_CPUS");
    assert(setenv("DATAROUTER_WAIT_MODE", "spin", 1) == 0);
    int available = datarouter_thread_attach();
    datarouter_thread_detach();
    cpu_set_t restored;
    assert(sched_getaffinity(0, sizeof(restored), &restored) == 0);
    assert(CPU_EQUAL(&original, &restored));
    if (available != 1) { puts("dedicated core unavailable; resource failure tested"); return 0; }
    pthread_t worker;
    assert(pthread_create(&worker, NULL, claim_and_exit, NULL) == 0);
    assert(pthread_join(worker, NULL) == 0);
    assert(datarouter_thread_attach() == 1);
    cpu_set_t reused;
    assert(sched_getaffinity(0, sizeof(reused), &reused) == 0);
    assert(CPU_EQUAL(&first, &reused));
    pid_t child = fork(); assert(child >= 0);
    if (!child) {
        cpu_set_t child_mask;
        int ok = sched_getaffinity(0, sizeof(child_mask), &child_mask) == 0 && CPU_EQUAL(&child_mask, &original);
        _exit(ok ? 0 : 1);
    }
    int status; assert(waitpid(child, &status, 0) == child);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    datarouter_thread_detach();
    assert(sched_getaffinity(0, sizeof(restored), &restored) == 0);
    assert(CPU_EQUAL(&original, &restored));
    /* A killed process must release its core without a stale lease file. */
    assert(datarouter_thread_attach() == 1);
    int pipefd[2]; assert(pipe(pipefd) == 0);
    child = fork(); assert(child >= 0);
    if (!child) {
        close(pipefd[0]);
        int chosen = -1;
        if (datarouter_thread_attach() == 1) {
            cpu_set_t mask;
            assert(sched_getaffinity(0, sizeof(mask), &mask) == 0);
            for (int i = 0; i < CPU_SETSIZE; i++) if (CPU_ISSET(i, &mask)) chosen = i;
        }
        assert(write(pipefd[1], &chosen, sizeof(chosen)) == sizeof(chosen));
        close(pipefd[1]);
        if (chosen < 0) _exit(0);
        for (;;) pause();
    }
    close(pipefd[1]); int chosen;
    assert(read(pipefd[0], &chosen, sizeof(chosen)) == sizeof(chosen)); close(pipefd[0]);
    datarouter_thread_detach();
    if (chosen >= 0) {
        snprintf(list, sizeof(list), "%d", chosen);
        assert(setenv("DATAROUTER_CPUS", list, 1) == 0);
        assert(datarouter_thread_attach() == -1); /* another process owns this core */
        datarouter_thread_detach();
        assert(kill(child, SIGKILL) == 0);
        assert(waitpid(child, &status, 0) == child && WIFSIGNALED(status));
        assert(datarouter_thread_attach() == 1);
        datarouter_thread_detach();
        assert(sched_getaffinity(0, sizeof(restored), &restored) == 0 && CPU_EQUAL(&original, &restored));
        unsetenv("DATAROUTER_CPUS");
    } else assert(waitpid(child, &status, 0) == child && WIFEXITED(status));
    datarouter_thread_detach(); /* idempotent */
    return 0;
}
