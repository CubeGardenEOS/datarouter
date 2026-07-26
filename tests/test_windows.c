#include "../src/datarouter_internal.h"
#include "../src/datarouter_signal.h"

#include <stdio.h>
#include <stdlib.h>

#ifdef _WIN32

static void require_ok(int condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "test_windows failed: %s\n", message);
        exit(1);
    }
}

int main(void) {
    const char *name = "/datarouter-windows-ci";
    size_t size = datarouter_shared_memory_size(4U);
    datarouter_mapping owner;
    require_ok(
        datarouter_platform_create(name, 0600, size, &owner) == DATAROUTER_OK,
        "create first mapping"
    );
    datarouter_mapping initializing;
    require_ok(
        datarouter_platform_open(name, 0U, &initializing) == DATAROUTER_ERR_AGAIN,
        "opening an unpublished mapping waits for initialization"
    );
    require_ok(
        datarouter_init_shared_memory(owner.shm, 4U) == DATAROUTER_OK,
        "initialize first mapping"
    );

    datarouter_mapping duplicate;
    require_ok(
        datarouter_platform_create(name, 0600, size, &duplicate) == DATAROUTER_ERR_BUSY,
        "second creator observes ERROR_ALREADY_EXISTS"
    );

    datarouter_mapping client;
    require_ok(datarouter_platform_open(name, 0U, &client) == DATAROUTER_OK, "open mapping");
    require_ok(
        datarouter_validate_shared_memory(client.shm, client.map_size) == DATAROUTER_OK,
        "validate opened mapping"
    );

    datarouter_signal signal;
    require_ok(datarouter_signal_init(&signal) == DATAROUTER_OK, "initialize signal");
    require_ok(
        datarouter_signal_wait(&signal, 1U) == DATAROUTER_ERR_TIMEOUT,
        "empty signal times out"
    );
    datarouter_signal_post(&signal);
    require_ok(datarouter_signal_wait(&signal, 10U) == DATAROUTER_OK, "posted signal wakes");
    datarouter_platform_close(&client);
    datarouter_platform_close(&owner);

    datarouter_mapping replacement;
    require_ok(
        datarouter_platform_create(name, 0600, size, &replacement) == DATAROUTER_OK,
        "mapping name can be reused after all handles close"
    );
    datarouter_platform_close(&replacement);
    puts("test_windows ok");
    return 0;
}

#else

int main(void) {
    return 0;
}

#endif
