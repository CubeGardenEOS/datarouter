#ifndef DATAROUTER_CONFIG_H
#define DATAROUTER_CONFIG_H

#include "datarouter_internal.h"

typedef struct {
    char allow_port[DATAROUTER_MAX_PORTS][DATAROUTER_SERVICE_NAME_MAX + 1U];
    uint32_t allow_port_count;
    int socket_raw;
    int socket_kcp;
} datarouter_config;

void datarouter_config_init(datarouter_config *config);
int datarouter_config_load(const char *path, datarouter_config *config, char *errbuf, size_t errbuf_len);
void datarouter_config_describe(const datarouter_config *config, char *buffer, size_t buffer_len);

#endif
