#include "config.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

void datarouter_config_init(datarouter_config *config) {
    if (!config) {
        return;
    }
    memset(config, 0, sizeof(*config));
}

static char *trim(char *value) {
    while (*value && isspace((unsigned char)*value)) {
        value++;
    }
    char *end = value + strlen(value);
    while (end > value && isspace((unsigned char)end[-1])) {
        *--end = '\0';
    }
    return value;
}

static int set_error(char *errbuf, size_t errbuf_len, const char *message) {
    datarouter_set_error(errbuf, errbuf_len, message);
    return DATAROUTER_ERR_INVALID_ARGUMENT;
}

static int parse_bool_flag(const char *value, int *out) {
    if (strcmp(value, "0") == 0) {
        *out = 0;
        return DATAROUTER_OK;
    }
    if (strcmp(value, "1") == 0) {
        *out = 1;
        return DATAROUTER_OK;
    }
    return DATAROUTER_ERR_INVALID_ARGUMENT;
}

static int has_duplicate(const datarouter_config *config, const char *name) {
    for (uint32_t i = 0; i < config->allow_port_count; i++) {
        if (strcmp(config->allow_port[i], name) == 0) {
            return 1;
        }
    }
    return 0;
}

static int parse_allow_port(datarouter_config *config, char *value, char *errbuf, size_t errbuf_len) {
    config->allow_port_count = 0U;
    char *cursor = value;
    while (1) {
        char *comma = strchr(cursor, ',');
        if (comma) {
            *comma = '\0';
        }
        char *token = trim(cursor);
        if (token[0] == '\0') {
            return set_error(errbuf, errbuf_len, "allow_port contains an empty service name");
        }
        if (!datarouter_valid_service_name(token)) {
            return set_error(errbuf, errbuf_len, "allow_port contains an invalid service name");
        }
        if (has_duplicate(config, token)) {
            return set_error(errbuf, errbuf_len, "allow_port contains a duplicate service name");
        }
        if (config->allow_port_count >= DATAROUTER_MAX_PORTS) {
            return set_error(errbuf, errbuf_len, "allow_port exceeds datarouter capacity");
        }
        snprintf(
            config->allow_port[config->allow_port_count],
            sizeof(config->allow_port[config->allow_port_count]),
            "%s",
            token
        );
        config->allow_port_count++;
        if (!comma) {
            break;
        }
        cursor = comma + 1;
    }
    return DATAROUTER_OK;
}

int datarouter_config_load(const char *path, datarouter_config *config, char *errbuf, size_t errbuf_len) {
    if (!path || !config) {
        return set_error(errbuf, errbuf_len, "missing config path");
    }
    FILE *file = fopen(path, "r");
    if (!file) {
        return set_error(errbuf, errbuf_len, "could not open config file");
    }

    datarouter_config_init(config);
    int in_common = 0;
    int saw_common = 0;
    int saw_allow_port = 0;
    int saw_socket_raw = 0;
    int saw_socket_kcp = 0;
    char line[512];
    while (fgets(line, sizeof(line), file)) {
        if (!strchr(line, '\n') && !feof(file)) {
            fclose(file);
            return set_error(errbuf, errbuf_len, "config line exceeds 511 bytes");
        }
        char *text = trim(line);
        if (text[0] == '\0' || text[0] == '#' || text[0] == ';') {
            continue;
        }
        if (text[0] == '[') {
            char *end = strchr(text, ']');
            if (!end || trim(end + 1)[0] != '\0') {
                fclose(file);
                return set_error(errbuf, errbuf_len, "invalid config section header");
            }
            *end = '\0';
            if (strcmp(text + 1, "common") != 0) {
                fclose(file);
                return set_error(errbuf, errbuf_len, "unknown config section");
            }
            if (saw_common) {
                fclose(file);
                return set_error(errbuf, errbuf_len, "duplicate [common] section");
            }
            in_common = 1;
            saw_common = 1;
            continue;
        }
        if (!in_common) {
            fclose(file);
            return set_error(errbuf, errbuf_len, "config keys must be in [common]");
        }
        char *equals = strchr(text, '=');
        if (!equals) {
            fclose(file);
            return set_error(errbuf, errbuf_len, "invalid config key-value line");
        }
        *equals = '\0';
        char *key = trim(text);
        char *value = trim(equals + 1);
        if (strcmp(key, "allow_port") == 0) {
            if (saw_allow_port) {
                fclose(file);
                return set_error(errbuf, errbuf_len, "duplicate allow_port key");
            }
            int rc = parse_allow_port(config, value, errbuf, errbuf_len);
            if (rc != DATAROUTER_OK) {
                fclose(file);
                return rc;
            }
            saw_allow_port = 1;
        } else if (strcmp(key, "socket_raw") == 0) {
            if (saw_socket_raw) {
                fclose(file);
                return set_error(errbuf, errbuf_len, "duplicate socket_raw key");
            }
            if (parse_bool_flag(value, &config->socket_raw) != DATAROUTER_OK) {
                fclose(file);
                return set_error(errbuf, errbuf_len, "socket_raw must be 0 or 1");
            }
            saw_socket_raw = 1;
        } else if (strcmp(key, "socket_kcp") == 0) {
            if (saw_socket_kcp) {
                fclose(file);
                return set_error(errbuf, errbuf_len, "duplicate socket_kcp key");
            }
            if (parse_bool_flag(value, &config->socket_kcp) != DATAROUTER_OK) {
                fclose(file);
                return set_error(errbuf, errbuf_len, "socket_kcp must be 0 or 1");
            }
            saw_socket_kcp = 1;
        } else {
            fclose(file);
            return set_error(errbuf, errbuf_len, "unknown config key");
        }
    }
    fclose(file);
    if (!saw_common || !saw_allow_port || config->allow_port_count == 0U) {
        return set_error(errbuf, errbuf_len, "config requires [common] allow_port");
    }
    return DATAROUTER_OK;
}

void datarouter_config_describe(const datarouter_config *config, char *buffer, size_t buffer_len) {
    if (!buffer || buffer_len == 0U) {
        return;
    }
    buffer[0] = '\0';
    if (!config) {
        return;
    }
    size_t used = 0U;
    for (uint32_t i = 0; i < config->allow_port_count; i++) {
        int written = snprintf(
            buffer + used,
            buffer_len - used,
            "%s%s",
            i == 0U ? "" : ",",
            config->allow_port[i]
        );
        if (written < 0 || (size_t)written >= buffer_len - used) {
            buffer[buffer_len - 1U] = '\0';
            return;
        }
        used += (size_t)written;
    }
}
