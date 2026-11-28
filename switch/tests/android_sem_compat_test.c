#define _POSIX_C_SOURCE 200809L
#include "android_sem_compat.h"

#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

// The Android object is opaque. No native sem_t is stored in its memory.
typedef struct { int32_t opaque[8]; } BionicSem;
static BionicSem shared;
static int (*guest_init)(void *, int, unsigned);
static int (*guest_wait)(void *);
static int (*guest_post)(void *);
static int (*guest_destroy)(void *);
static unsigned protected_count;

static void *count_worker(void *unused)
{
    (void)unused;
    for (unsigned i = 0; i < 1000; i++) {
        assert(guest_wait(&shared) == 0);
        protected_count++;
        assert(guest_post(&shared) == 0);
    }
    return NULL;
}

static void *blocked_worker(void *opaque)
{
    assert(guest_wait(opaque) == 0);
    return NULL;
}

static uint64_t lookup(const char *name)
{
    uint64_t result = 0;
    assert(android_sem_compat_resolve(name, "LIBC", "libc.so", &result));
    assert(result);
    assert(!android_sem_compat_resolve(name, "LIBC_N", "libc.so", &result));
    assert(!android_sem_compat_resolve(name, "LIBC", "libm.so", &result));
    return result;
}

int main(void)
{
    guest_init = (int (*)(void *, int, unsigned))(uintptr_t)lookup("sem_init");
    guest_wait = (int (*)(void *))(uintptr_t)lookup("sem_wait");
    guest_post = (int (*)(void *))(uintptr_t)lookup("sem_post");
    guest_destroy = (int (*)(void *))(uintptr_t)lookup("sem_destroy");
    uint64_t unknown = 42;
    assert(!android_sem_compat_resolve("sem_timedwait", "LIBC", "libc.so", &unknown));
    assert(unknown == 42);
    assert(guest_init(NULL, 0, 1) == -1 && errno == EINVAL);
    assert(guest_init(&shared, 1, 1) == -1 && errno == ENOSYS);
    assert(guest_init(&shared, 0, UINT_MAX) == -1 && errno == EINVAL);
    assert(guest_init(&shared, 0, 1) == 0);
    assert(guest_init(&shared, 0, 1) == -1 && errno == EBUSY);
    pthread_t threads[6];
    for (unsigned i = 0; i < 6; i++)
        assert(pthread_create(&threads[i], NULL, count_worker, NULL) == 0);
    for (unsigned i = 0; i < 6; i++)
        assert(pthread_join(threads[i], NULL) == 0);
    assert(protected_count == 6000);
    assert(guest_destroy(&shared) == 0);
    assert(guest_wait(&shared) == -1 && errno == EINVAL);
    assert(guest_post(&shared) == -1 && errno == EINVAL);
    assert(guest_destroy(&shared) == -1 && errno == EINVAL);

    BionicSem zero = {{0}};
    assert(guest_init(&zero, 0, 0) == 0);
    pthread_t waiting;
    assert(pthread_create(&waiting, NULL, blocked_worker, &zero) == 0);
    struct timespec pause = {0, 10000000};
    nanosleep(&pause, NULL);
    assert(guest_destroy(&zero) == -1 && errno == EBUSY);
    assert(guest_post(&zero) == 0);
    assert(pthread_join(waiting, NULL) == 0);
    assert(guest_destroy(&zero) == 0);

    BionicSem maximum = {{0}};
    assert(guest_init(&maximum, 0, INT_MAX) == 0);
    assert(guest_post(&maximum) == -1 && errno == EOVERFLOW);
    assert(guest_destroy(&maximum) == 0);
    puts("android_sem_compat_test: counting, contention and teardown passed");
    return 0;
}
