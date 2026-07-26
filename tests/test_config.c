#include "../src/config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void require_int(int ok, const char *message) {
    if (!ok) {
        fprintf(stderr, "test_config failed: %s\n", message);
        exit(1);
    }
}

static void write_file(const char *path, const char *text) {
    FILE *file = fopen(path, "w");
    require_int(file != NULL, "open temp config");
    require_int(fputs(text, file) >= 0, "write temp config");
    fclose(file);
}

static int load_text(const char *text, datarouter_config *config, char *err, size_t err_len) {
    const char *path = "build/test_config.ini";
    write_file(path, text);
    return datarouter_config_load(path, config, err, err_len);
}

int main(void) {
    datarouter_config config;
    char err[128];

    require_int(
        load_text("[common]\nallow_port=core,ml_manager\n", &config, err, sizeof(err)) == DATAROUTER_OK,
        "valid allow_port"
    );
    require_int(config.allow_port_count == 2U, "valid count");
    require_int(strcmp(config.allow_port[0], "core") == 0, "first name");
    require_int(strcmp(config.allow_port[1], "ml_manager") == 0, "second name");

    require_int(
        load_text("[common]\nallow_port=Core,core\n", &config, err, sizeof(err)) == DATAROUTER_OK,
        "case-sensitive names preserved"
    );
    require_int(strcmp(config.allow_port[0], "Core") == 0, "upper-case preserved");
    require_int(strcmp(config.allow_port[1], "core") == 0, "lower-case preserved");

    require_int(
        load_text("[common]\nallow_port=core,core\n", &config, err, sizeof(err)) == DATAROUTER_ERR_INVALID_ARGUMENT,
        "duplicate rejected"
    );
    require_int(
        load_text("[common]\nallow_port=core,,ml\n", &config, err, sizeof(err)) == DATAROUTER_ERR_INVALID_ARGUMENT,
        "empty token rejected"
    );
    require_int(
        load_text("[common]\nallow_port=core,bad/name\n", &config, err, sizeof(err)) == DATAROUTER_ERR_INVALID_ARGUMENT,
        "invalid service name rejected"
    );
    require_int(
        load_text("[common]\nallow_port=core\nuser=nobody\n", &config, err, sizeof(err)) ==
            DATAROUTER_ERR_INVALID_ARGUMENT,
        "unknown key rejected"
    );
    require_int(
        load_text("[common]\nallow_port=core\nsocket_raw=1\nsocket_kcp=0\n", &config, err, sizeof(err)) ==
            DATAROUTER_OK,
        "socket flags accepted"
    );
    require_int(config.socket_raw == 1 && config.socket_kcp == 0, "socket flag values stored");
    require_int(
        load_text("[common]\nallow_port=core\nallow_port=ml\n", &config, err, sizeof(err)) ==
            DATAROUTER_ERR_INVALID_ARGUMENT,
        "duplicate allow_port rejected"
    );
    require_int(
        load_text("[common]\nallow_port=core\n[common]\n", &config, err, sizeof(err)) ==
            DATAROUTER_ERR_INVALID_ARGUMENT,
        "duplicate common section rejected"
    );

    remove("build/test_config.ini");
    printf("test_config ok\n");
    return 0;
}
