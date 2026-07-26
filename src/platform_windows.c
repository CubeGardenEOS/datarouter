#include "datarouter_internal.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdio.h>
#include <string.h>

static int mapping_name(const char *name, char *buffer, size_t buffer_len) {
    if (!datarouter_valid_shm_name(name) || !buffer || buffer_len == 0U) {
        return DATAROUTER_ERR_INVALID_ARGUMENT;
    }
    int written = snprintf(buffer, buffer_len, "Local\\%s", name + 1);
    if (written <= 0 || (size_t)written >= buffer_len) {
        buffer[0] = '\0';
        return DATAROUTER_ERR_INVALID_ARGUMENT;
    }
    return DATAROUTER_OK;
}

int datarouter_platform_create(
    const char *name,
    mode_t mode,
    size_t size,
    datarouter_mapping *out
) {
    (void)mode;
    if (!datarouter_valid_shm_name(name) || !out || size == 0U) {
        return DATAROUTER_ERR_INVALID_ARGUMENT;
    }
    memset(out, 0, sizeof(*out));
    out->fd = -1;

    char win_name[128];
    int name_rc = mapping_name(name, win_name, sizeof(win_name));
    if (name_rc != DATAROUTER_OK) {
        return name_rc;
    }
    HANDLE handle = CreateFileMappingA(
        INVALID_HANDLE_VALUE,
        NULL,
        PAGE_READWRITE,
        (DWORD)((uint64_t)size >> 32U),
        (DWORD)((uint64_t)size & 0xffffffffU),
        win_name
    );
    if (!handle) {
        return DATAROUTER_ERR_PLATFORM;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(handle);
        return DATAROUTER_ERR_BUSY;
    }
    void *ptr = MapViewOfFile(handle, FILE_MAP_ALL_ACCESS, 0, 0, size);
    if (!ptr) {
        CloseHandle(handle);
        return DATAROUTER_ERR_PLATFORM;
    }
    out->fd = -1;
    out->map_size = size;
    out->shm = (datarouter_shared_memory *)ptr;
    out->owner = 1;
    out->platform_handle = handle;
    return DATAROUTER_OK;
}

int datarouter_platform_open(const char *name, size_t size_hint, datarouter_mapping *out) {
    if (!datarouter_valid_shm_name(name) || !out) {
        return DATAROUTER_ERR_INVALID_ARGUMENT;
    }
    memset(out, 0, sizeof(*out));
    out->fd = -1;

    char win_name[128];
    int name_rc = mapping_name(name, win_name, sizeof(win_name));
    if (name_rc != DATAROUTER_OK) {
        return name_rc;
    }
    HANDLE handle = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, win_name);
    if (!handle) {
        return GetLastError() == ERROR_FILE_NOT_FOUND ? DATAROUTER_ERR_NOT_FOUND : DATAROUTER_ERR_PLATFORM;
    }

    size_t map_size = size_hint == 0U ? sizeof(datarouter_control_plane) : size_hint;
    void *ptr = MapViewOfFile(handle, FILE_MAP_ALL_ACCESS, 0, 0, map_size);
    if (!ptr) {
        CloseHandle(handle);
        return DATAROUTER_ERR_PLATFORM;
    }

    if (size_hint == 0U) {
        datarouter_shared_memory *header = (datarouter_shared_memory *)ptr;
        if (atomic_load_explicit(&header->control.initialized, memory_order_acquire) != 1U) {
            UnmapViewOfFile(ptr);
            CloseHandle(handle);
            return DATAROUTER_ERR_AGAIN;
        }
        size_t total_size = header->control.total_size;
        size_t maximum_size = datarouter_shared_memory_size(DATAROUTER_MAX_PORTS);
        if (header->control.magic != DATAROUTER_MAGIC ||
            header->control.layout_version != DATAROUTER_LAYOUT_VERSION ||
            total_size < sizeof(datarouter_control_plane) || total_size > maximum_size) {
            UnmapViewOfFile(ptr);
            CloseHandle(handle);
            return DATAROUTER_ERR_PROTOCOL;
        }
        map_size = total_size;
        UnmapViewOfFile(ptr);
        ptr = MapViewOfFile(handle, FILE_MAP_ALL_ACCESS, 0, 0, map_size);
        if (!ptr) {
            CloseHandle(handle);
            return DATAROUTER_ERR_PLATFORM;
        }
    }

    out->fd = -1;
    out->map_size = map_size;
    out->shm = (datarouter_shared_memory *)ptr;
    out->owner = 0;
    out->platform_handle = handle;
    return DATAROUTER_OK;
}

int datarouter_platform_unlink(const char *name) {
    return datarouter_valid_shm_name(name) ? DATAROUTER_OK : DATAROUTER_ERR_INVALID_ARGUMENT;
}

void datarouter_platform_close(datarouter_mapping *mapping) {
    if (!mapping) {
        return;
    }
    if (mapping->shm) {
        UnmapViewOfFile(mapping->shm);
    }
    if (mapping->platform_handle) {
        CloseHandle((HANDLE)mapping->platform_handle);
    }
    mapping->fd = -1;
    mapping->map_size = 0U;
    mapping->shm = NULL;
    mapping->owner = 0;
    mapping->platform_handle = NULL;
}
