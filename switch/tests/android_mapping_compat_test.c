#define _POSIX_C_SOURCE 200809L
#include "android_mapping_compat.h"
#include <assert.h>
#include <errno.h>
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
        assert(unmap_data(p, 8191) == 0);
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
    assert(map_data(NULL, SIZE_MAX, 3, 0x22, -1, 0) == FAILED);
    assert(map_data(NULL, 4096, 7, 0x22, -1, 0) == FAILED);
    assert(map_data(NULL, 4096, 3, 0x32, -1, 0) == FAILED);
    assert(map_data(NULL, 4096, 3, 2, 0, 0) == FAILED);
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
    pthread_t threads[8];
    for (uintptr_t i = 0; i < 8; ++i)
        assert(pthread_create(&threads[i], NULL, worker, (void *)i) == 0);
    for (unsigned i = 0; i < 8; ++i) assert(pthread_join(threads[i], NULL) == 0);
    puts("anonymous mappings, permission faults and concurrent release passed");
}
