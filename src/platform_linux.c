#include "datarouter_internal.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

static int map_fd(int fd, size_t size, datarouter_mapping *out) {
    void *ptr = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (ptr == MAP_FAILED) {
        return DATAROUTER_ERR_PLATFORM;
    }
    out->fd = fd;
    out->map_size = size;
    out->shm = (datarouter_shared_memory *)ptr;
    return DATAROUTER_OK;
}

int datarouter_platform_create(
    const char *name,
    mode_t mode,
    size_t size,
    datarouter_mapping *out
) {
    if (!datarouter_valid_shm_name(name) || !out || size == 0U || size > (size_t)INT64_MAX) {
        return DATAROUTER_ERR_INVALID_ARGUMENT;
    }
    memset(out, 0, sizeof(*out));
    out->fd = -1;

    int fd = shm_open(name, O_CREAT | O_EXCL | O_RDWR, mode);
    if (fd < 0) {
        if (errno == EEXIST) {
            return DATAROUTER_ERR_BUSY;
        }
        return errno == EACCES ? DATAROUTER_ERR_PERMISSION : DATAROUTER_ERR_PLATFORM;
    }
    if (fchmod(fd, mode) != 0) {
        close(fd);
        (void)shm_unlink(name);
        return DATAROUTER_ERR_PLATFORM;
    }
    if (ftruncate(fd, (off_t)size) != 0) {
        close(fd);
        (void)shm_unlink(name);
        return DATAROUTER_ERR_PLATFORM;
    }
    int rc = map_fd(fd, size, out);
    if (rc != DATAROUTER_OK) {
        close(fd);
        (void)shm_unlink(name);
        return rc;
    }
    out->owner = 1;
    return DATAROUTER_OK;
}

int datarouter_platform_open(const char *name, size_t size_hint, datarouter_mapping *out) {
    if (!datarouter_valid_shm_name(name) || !out) {
        return DATAROUTER_ERR_INVALID_ARGUMENT;
    }
    memset(out, 0, sizeof(*out));
    out->fd = -1;

    int fd = shm_open(name, O_RDWR, 0);
    if (fd < 0) {
        if (errno == ENOENT) {
            return DATAROUTER_ERR_NOT_FOUND;
        }
        return errno == EACCES ? DATAROUTER_ERR_PERMISSION : DATAROUTER_ERR_PLATFORM;
    }
    size_t size = size_hint;
    if (size == 0U) {
        struct stat st;
        if (fstat(fd, &st) != 0 || st.st_size <= 0 || (uintmax_t)st.st_size > SIZE_MAX) {
            close(fd);
            return DATAROUTER_ERR_PLATFORM;
        }
        size = (size_t)st.st_size;
    }
    int rc = map_fd(fd, size, out);
    if (rc != DATAROUTER_OK) {
        close(fd);
        return rc;
    }
    out->owner = 0;
    return DATAROUTER_OK;
}

int datarouter_platform_unlink(const char *name) {
    if (!datarouter_valid_shm_name(name)) {
        return DATAROUTER_ERR_INVALID_ARGUMENT;
    }
    if (shm_unlink(name) == 0 || errno == ENOENT) {
        return DATAROUTER_OK;
    }
    return errno == EACCES || errno == EPERM
        ? DATAROUTER_ERR_PERMISSION
        : DATAROUTER_ERR_PLATFORM;
}

void datarouter_platform_close(datarouter_mapping *mapping) {
    if (!mapping) {
        return;
    }
    if (mapping->shm) {
        munmap(mapping->shm, mapping->map_size);
    }
    if (mapping->fd >= 0) {
        close(mapping->fd);
    }
    mapping->fd = -1;
    mapping->map_size = 0U;
    mapping->shm = NULL;
    mapping->owner = 0;
    mapping->platform_handle = NULL;
}
