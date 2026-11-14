#define _POSIX_C_SOURCE 200809L
#include "android_asset_compat.h"

#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

enum { MAX_MANAGERS = 8, ASSET_PATH_BYTES = 512,
       MAX_ASSET_BUFFER = 16 * 1024 * 1024 };

typedef struct {
    void *java_manager;
    char root[ASSET_PATH_BYTES];
    AndroidAssetConfiguration configuration;
    bool configuration_set;
    bool bound;
} NativeAssetManager;

typedef struct {
    char path[ASSET_PATH_BYTES];
    off_t length;
    unsigned char *buffer;
} NativeAsset;

static NativeAssetManager *managers[MAX_MANAGERS];
static pthread_mutex_t managers_lock = PTHREAD_MUTEX_INITIALIZER;

static NativeAssetManager *find_java_manager(void *java_manager)
{
    for (unsigned i = 0; i < MAX_MANAGERS; i++)
        if (managers[i] && managers[i]->bound &&
            managers[i]->java_manager == java_manager)
            return managers[i];
    return NULL;
}

static bool valid_asset_name(const char *name)
{
    if (!name || !*name) return false;
    const char *part = name;
    for (size_t i = 0; i < ASSET_PATH_BYTES; i++) {
        const char ch = name[i];
        if (ch == '\0') {
            const size_t len = name + i - part;
            return len && !(len == 1 && part[0] == '.') &&
                   !(len == 2 && part[0] == '.' && part[1] == '.');
        }
        if (ch == '/' || ch == '\\' || ch == ':') {
            if (ch != '/') return false;
            const size_t len = name + i - part;
            if (!len || (len == 1 && part[0] == '.') ||
                (len == 2 && part[0] == '.' && part[1] == '.'))
                return false;
            part = name + i + 1;
        }
    }
    return false;
}

bool android_asset_compat_bind_manager(void *java_manager, const char *root)
{
    if (!java_manager || !root || !*root) return false;
    const size_t length = strlen(root);
    if (length + 2 >= ASSET_PATH_BYTES) return false;
    struct stat st;
    if (stat(root, &st) != 0 || !S_ISDIR(st.st_mode)) return false;

    pthread_mutex_lock(&managers_lock);
    NativeAssetManager *manager = find_java_manager(java_manager);
    if (!manager) {
        for (unsigned i = 0; i < MAX_MANAGERS; i++) {
            if (managers[i]) continue;
            manager = calloc(1, sizeof(*manager));
            if (manager) managers[i] = manager;
            break;
        }
    }
    if (manager) {
        // Rebinding a Java object cannot inherit a previous owner's locale.
        if (!manager->bound || manager->java_manager != java_manager) {
            memset(&manager->configuration, 0, sizeof(manager->configuration));
            manager->configuration_set = false;
        }
        manager->java_manager = java_manager;
        manager->bound = true;
        memcpy(manager->root, root, length + 1);
    }
    pthread_mutex_unlock(&managers_lock);
    return manager != NULL;
}

void android_asset_compat_unbind_manager(void *java_manager)
{
    if (!java_manager) return;
    pthread_mutex_lock(&managers_lock);
    NativeAssetManager *manager = find_java_manager(java_manager);
    if (manager) {
        manager->bound = false;
        manager->java_manager = NULL;
        manager->root[0] = 0;
        memset(&manager->configuration, 0, sizeof(manager->configuration));
        manager->configuration_set = false;
        // Keep the unique handle allocated so stale guest handles cannot be
        // mistaken for a different Java object reusing the same address.
    }
    pthread_mutex_unlock(&managers_lock);
}

bool android_asset_compat_set_configuration(void *candidate,
                                             const AndroidAssetConfiguration *configuration)
{
    if (!candidate || !configuration) return false;
    pthread_mutex_lock(&managers_lock);
    bool found = false;
    for (unsigned i = 0; i < MAX_MANAGERS; i++) {
        if (!managers[i] || managers[i] != candidate || !managers[i]->bound) continue;
        managers[i]->configuration = *configuration;
        managers[i]->configuration_set = true;
        found = true;
        break;
    }
    pthread_mutex_unlock(&managers_lock);
    return found;
}

bool android_asset_compat_get_configuration(const void *candidate,
                                             AndroidAssetConfiguration *configuration)
{
    if (!configuration) return false;
    memset(configuration, 0, sizeof(*configuration));
    if (!candidate) return false;
    pthread_mutex_lock(&managers_lock);
    bool found = false;
    for (unsigned i = 0; i < MAX_MANAGERS; i++) {
        if (!managers[i] || managers[i] != candidate || !managers[i]->bound ||
            !managers[i]->configuration_set) continue;
        *configuration = managers[i]->configuration;
        found = true;
        break;
    }
    pthread_mutex_unlock(&managers_lock);
    return found;
}

static NativeAssetManager *manager_from_java(void *env, void *java_manager)
{
    if (!env || !java_manager) return NULL;
    pthread_mutex_lock(&managers_lock);
    NativeAssetManager *manager = find_java_manager(java_manager);
    pthread_mutex_unlock(&managers_lock);
    return manager;
}

static NativeAsset *manager_open(NativeAssetManager *manager, const char *name,
                                 int mode)
{
    if (!manager || !valid_asset_name(name) || mode < 0 || mode > 3) return NULL;
    char root[ASSET_PATH_BYTES] = {0};
    pthread_mutex_lock(&managers_lock);
    for (unsigned i = 0; i < MAX_MANAGERS; i++) {
        if (managers[i] == manager && manager->bound) {
            memcpy(root, manager->root, sizeof(root));
            break;
        }
    }
    pthread_mutex_unlock(&managers_lock);
    if (!root[0]) return NULL;
    NativeAsset *asset = calloc(1, sizeof(*asset));
    if (!asset) return NULL;
    const int n = snprintf(asset->path, sizeof(asset->path), "%s/%s", root, name);
    struct stat st;
    if (n <= 0 || (size_t)n >= sizeof(asset->path) ||
        stat(asset->path, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0) {
        free(asset);
        return NULL;
    }
    asset->length = st.st_size;
    return asset;
}

static const void *asset_buffer(NativeAsset *asset)
{
    if (!asset) return NULL;
    if (asset->buffer) return asset->buffer;
    if (asset->length > MAX_ASSET_BUFFER) return NULL;
    const size_t length = (size_t)asset->length;
    FILE *file = fopen(asset->path, "rb");
    if (!file) return NULL;
    unsigned char *buffer = malloc(length ? length : 1);
    if (!buffer || fread(buffer, 1, length, file) != length || ferror(file)) {
        free(buffer);
        fclose(file);
        return NULL;
    }
    fclose(file);
    asset->buffer = buffer;
    return buffer;
}

static off_t asset_length(NativeAsset *asset)
{
    return asset ? asset->length : 0;
}

static int asset_descriptor(NativeAsset *asset, off_t *start, off_t *length)
{
    if (!asset || !start || !length) return -1;
    int fd = open(asset->path, O_RDONLY);
    if (fd < 0) return -1;
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size != asset->length) {
        close(fd);
        return -1;
    }
    *start = 0;
    *length = asset->length;
    return fd;
}

static void asset_close(NativeAsset *asset)
{
    if (!asset) return;
    free(asset->buffer);
    free(asset);
}

bool android_asset_compat_resolve(const char *name, const char *version,
                                  const char *library, uint64_t *address)
{
    if (!name || !address || version || library) return false;
    *address = 0;
#define ASSET_MAP(symbol, implementation) \
    if (strcmp(name, #symbol) == 0) \
        *address = (uint64_t)(uintptr_t)&implementation; else
    ASSET_MAP(AAssetManager_fromJava, manager_from_java)
    ASSET_MAP(AAssetManager_open, manager_open)
    ASSET_MAP(AAsset_getBuffer, asset_buffer)
    ASSET_MAP(AAsset_getLength, asset_length)
    ASSET_MAP(AAsset_openFileDescriptor, asset_descriptor)
    ASSET_MAP(AAsset_close, asset_close)
    (void)0;
#undef ASSET_MAP
    return *address != 0;
}
