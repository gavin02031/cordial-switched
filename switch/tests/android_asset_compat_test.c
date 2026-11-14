#define _POSIX_C_SOURCE 200809L
#include "android_asset_compat.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

typedef void *(*FromJava)(void *, void *);
typedef void *(*OpenAsset)(void *, const char *, int);
typedef const void *(*GetBuffer)(void *);
typedef off_t (*GetLength)(void *);
typedef int (*OpenDescriptor)(void *, off_t *, off_t *);
typedef void (*CloseAsset)(void *);

static void resolve(const char *name, void *target)
{
    uint64_t address = 0;
    assert(android_asset_compat_resolve(name, NULL, NULL, &address) && address);
    memcpy(target, &address, sizeof(address));
}

int main(void)
{
    FromJava from_java;
    OpenAsset open_asset;
    GetBuffer get_buffer;
    GetLength get_length;
    OpenDescriptor open_descriptor;
    CloseAsset close_asset;
    resolve("AAssetManager_fromJava", &from_java);
    resolve("AAssetManager_open", &open_asset);
    resolve("AAsset_getBuffer", &get_buffer);
    resolve("AAsset_getLength", &get_length);
    resolve("AAsset_openFileDescriptor", &open_descriptor);
    resolve("AAsset_close", &close_asset);

    uint64_t bad = 0;
    assert(!android_asset_compat_resolve("AAssetManager_open", "LIBC", "libc.so", &bad));
    assert(!android_asset_compat_resolve("AAsset_read", NULL, NULL, &bad));

    char root[] = "/tmp/morrow-asset-XXXXXX";
    assert(mkdtemp(root));
    char path[256];
    assert(snprintf(path, sizeof(path), "%s/hello.bin", root) > 0);
    FILE *file = fopen(path, "wb");
    assert(file);
    assert(fwrite("morrow", 1, 6, file) == 6);
    assert(fclose(file) == 0);

    int java_asset_manager = 1, env = 2;
    assert(from_java(&env, &java_asset_manager) == NULL);
    assert(!android_asset_compat_bind_manager(NULL, root));
    assert(!android_asset_compat_bind_manager(&java_asset_manager, "/nonexistent"));
    assert(android_asset_compat_bind_manager(&java_asset_manager, root));
    assert(from_java(NULL, &java_asset_manager) == NULL);
    void *manager = from_java(&env, &java_asset_manager);
    assert(manager);
    assert(open_asset(manager, "../hello.bin", 0) == NULL);
    assert(open_asset(manager, "foo/../hello.bin", 0) == NULL);
    assert(open_asset(manager, "/hello.bin", 0) == NULL);
    assert(open_asset(manager, "x\\hello.bin", 0) == NULL);
    assert(open_asset(manager, "hello.bin", 4) == NULL);
    assert(open_asset(manager, "missing.bin", 0) == NULL);
    void *asset = open_asset(manager, "hello.bin", 3);
    assert(asset && get_length(asset) == 6);
    const void *buffer = get_buffer(asset);
    assert(buffer && memcmp(buffer, "morrow", 6) == 0);
    assert(get_buffer(asset) == buffer);
    off_t start = -1, length = -1;
    int fd = open_descriptor(asset, &start, &length);
    assert(fd >= 0 && start == 0 && length == 6);
    char observed[7] = {0};
    assert(read(fd, observed, 6) == 6 && strcmp(observed, "morrow") == 0);
    assert(close(fd) == 0);
    android_asset_compat_unbind_manager(&java_asset_manager);
    assert(from_java(&env, &java_asset_manager) == NULL);
    assert(open_asset(manager, "hello.bin", 0) == NULL);
    assert(memcmp(get_buffer(asset), "morrow", 6) == 0);
    close_asset(asset);
    assert(unlink(path) == 0);
    assert(rmdir(root) == 0);
    puts("android_asset_compat_test: all assertions passed");
    return 0;
}
