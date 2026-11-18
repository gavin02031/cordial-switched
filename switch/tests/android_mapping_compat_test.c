#define _POSIX_C_SOURCE 200809L
#include "android_mapping_compat.h"
#include <assert.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

static void *(*map_data)(void *, size_t, int, int, int, int64_t);
static int (*protect_data)(void *, size_t, int);
static int (*unmap_data)(void *, size_t);
static void *(*resize_data)(void *, size_t, size_t, int, ...);
static int (*discard_data)(void *, size_t, int);
#define FAILED ((void *)(intptr_t)-1)

static void *worker(void *arg)
{
    for (unsigned round = 0; round < 32; ++round) {
        unsigned char *p = map_data(NULL, 8191, 3, 0x22, -1, 0);
        assert(p != FAILED && (uintptr_t)p % 4096 == 0);
        assert(p[0] == 0 && p[8191] == 0);
        p[0] = (unsigned char)(uintptr_t)arg;
        p[8191] = 47;
        assert(protect_data(p + 4096, 1, 1) == 0);
        assert(unmap_data(p, 8193) == -1);
        assert(p[8191] == 47 && p[0] == (uintptr_t)arg);
        assert(resize_data(p, 8191, 12288, 1) == FAILED && errno == ENOTSUP);
        assert(protect_data(p, 8191, 3) == 0);
        assert(resize_data(p, 8191, 12288, 0) == FAILED && errno == ENOMEM);
        unsigned char *grown = resize_data(p, 8191, 12288, 1);
        assert(grown != FAILED && grown != p);
        assert(grown[0] == (uintptr_t)arg && grown[8191] == 47);
        assert(grown[8192] == 0 && grown[12287] == 0);
        assert(discard_data(grown + 4096, 1, 4) == 0);
        assert(grown[8191] == 0 && grown[0] == (uintptr_t)arg);
        assert(resize_data(grown, 12288, 4096, 0) == grown);
        assert(unmap_data(grown, 4096) == 0);
    }
    return NULL;
}

int main(void)
{
    uint64_t value;
    assert(!android_mapping_compat_resolve("mmap", "LIBC_N", "libc.so", &value));
    assert(android_mapping_compat_resolve("mmap", "LIBC", "libc.so", &value));
    map_data = (void *)(uintptr_t)value;
    assert(android_mapping_compat_resolve("mprotect", "LIBC", "libc.so", &value));
    protect_data = (void *)(uintptr_t)value;
    assert(android_mapping_compat_resolve("munmap", "LIBC", "libc.so", &value));
    unmap_data = (void *)(uintptr_t)value;
    assert(android_mapping_compat_resolve("mremap", "LIBC", "libc.so", &value));
    resize_data = (void *)(uintptr_t)value;
    assert(android_mapping_compat_resolve("madvise", "LIBC", "libc.so", &value));
    discard_data = (void *)(uintptr_t)value;
    assert(map_data(NULL, SIZE_MAX, 3, 0x22, -1, 0) == FAILED);
    assert(map_data(NULL, 4096, 7, 0x22, -1, 0) == FAILED);
    assert(map_data(NULL, 4096, 3, 0x32, -1, 0) == FAILED);
    assert(map_data(NULL, 4096, 3, 2, -1, 0) == FAILED);
    unsigned char *p = map_data(NULL, 4096, 0, 0x22, -1, 0);
    assert(p != FAILED);
    struct rlimit limit = {0, 0};
    assert(setrlimit(RLIMIT_CORE, &limit) == 0);
    for (int perm = 0; perm <= 1; perm++) {
        assert(protect_data(p, 4096, perm) == 0);
        pid_t child = fork();
        assert(child >= 0);
        if (!child) { *(volatile unsigned char *)p = 1; _exit(1); }
        int status;
        assert(waitpid(child, &status, 0) == child);
        assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGSEGV);
    }
    assert(protect_data(p, 4097, 3) == -1);
    assert(protect_data(p, 4096, 3) == 0);
    p[0] = 99;
    assert(unmap_data(p, 4096) == 0);
    assert(protect_data(p, 4096, 3) == -1);
    p = map_data(NULL, 12288, 3, 0x22, -1, 0);
    assert(p != FAILED);
    p[0] = 11;
    p[8192] = 22;
    assert(unmap_data(p + 4096, 4096) == 0);
    assert(protect_data(p + 4096, 1, 1) == -1);
    assert(p[0] == 11 && p[8192] == 22);
    assert(unmap_data(p + 8192, 1) == 0);
    assert(unmap_data(p, 4096) == 0);
    assert(android_mapping_compat_resolve("msync", "LIBC", "libc.so", &value));
    int (*sync_data)(void *, size_t, int) = (void *)(uintptr_t)value;
    char path[] = "/tmp/morrow-map-XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0 && unlink(path) == 0);
    assert(pwrite(fd, "hello", 5, 4096) == 5);
    assert(lseek(fd, 37, SEEK_SET) == 37);
    assert(map_data(NULL, 8192, 3, 2, fd, 4096) == FAILED);
    p = map_data(NULL, 5, 3, 2, fd, 4096);
    assert(p != FAILED && !memcmp(p, "hello", 5));
    assert(lseek(fd, 0, SEEK_CUR) == 37 && p[5] == 0 && p[4095] == 0);
    p[0] = 'j';
    assert(sync_data(p, 5, 4) == 0);
    assert(sync_data(p, 5, 5) == -1 && errno == EINVAL);
    char source[5];
    assert(pread(fd, source, 5, 4096) == 5 && !memcmp(source, "hello", 5));
    assert(close(fd) == 0);
    assert(!memcmp(p, "jello", 5));
    assert(discard_data(p, 5, 4) == -1);
    assert(resize_data(p, 5, 8192, 1) == FAILED);
    assert(unmap_data(p, 5) == 0);
    // With no other allocating threads, the released address cannot already
    // belong to a different guest mapping.
    p = map_data(NULL, 8192, 3, 0x22, -1, 0);
    assert(p != FAILED && resize_data(p, 8192, 4096, 0) == p);
    assert(protect_data(p + 4096, 4096, 3) == -1 && errno == ENOMEM);
    assert(unmap_data(p, 4096) == 0);
    pthread_t threads[8];
    for (uintptr_t i = 0; i < 8; ++i)
        assert(pthread_create(&threads[i], NULL, worker, (void *)i) == 0);
    for (unsigned i = 0; i < 8; ++i) assert(pthread_join(threads[i], NULL) == 0);
    puts("anonymous mappings, permission faults and concurrent release passed");
}
