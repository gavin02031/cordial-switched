#define _POSIX_C_SOURCE 200809L
#include "android_tls_compat.h"

#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>

enum { KEY_COUNT = 128, WORKER_COUNT = 8 };
static int (*bionic_key_create)(uint32_t *, void (*)(void *));
static int (*bionic_key_delete)(uint32_t);
static int (*bionic_setspecific)(uint32_t, const void *);
static void *(*bionic_getspecific)(uint32_t);
static uint32_t keys[KEY_COUNT];
static unsigned destroyed_values;
static unsigned cleared_before_destructor;

static void destructor_rearm(void *value)
{
    assert(bionic_getspecific(keys[0]) == NULL);
    __atomic_add_fetch(&destroyed_values, 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&cleared_before_destructor, 1, __ATOMIC_RELAXED);
    if ((uintptr_t)value == 1)
        assert(bionic_setspecific(keys[0], (void *)(uintptr_t)2) == 0);
    else
        assert((uintptr_t)value == 2);
}

static void *worker(void *argument)
{
    uintptr_t thread_number = (uintptr_t)argument;
    assert(bionic_getspecific(keys[0]) == NULL);
    assert(bionic_setspecific(keys[0], (void *)(uintptr_t)1) == 0);
    assert(bionic_setspecific(keys[1], (void *)(thread_number + 10)) == 0);
    for (unsigned i = 0; i < 500; i++) {
        assert((uintptr_t)bionic_getspecific(keys[0]) == 1);
        assert((uintptr_t)bionic_getspecific(keys[1]) == thread_number + 10);
    }
    return NULL;
}

static uint64_t lookup(const char *name)
{
    uint64_t address = 0;
    assert(android_tls_compat_resolve(name, "LIBC", "libc.so", &address));
    return address;
}

#define RESOLVE(name, type) ((type)(uintptr_t)lookup(name))

int main(void)
{
    uint64_t address = 0;
    assert(!android_tls_compat_resolve("pthread_key_create", "LIBM", "libc.so", &address));
    assert(!android_tls_compat_resolve("pthread_key_create", "LIBC", "libm.so", &address));
    assert(!android_tls_compat_resolve("pthread_self", "LIBC", "libc.so", &address));
    assert(!android_tls_compat_resolve("pthread_setspecific", "LIBC", "libc.so", NULL));

    bionic_key_create = RESOLVE("pthread_key_create", int (*)(uint32_t *, void (*)(void *)));
    bionic_key_delete = RESOLVE("pthread_key_delete", int (*)(uint32_t));
    bionic_setspecific = RESOLVE("pthread_setspecific", int (*)(uint32_t, const void *));
    bionic_getspecific = RESOLVE("pthread_getspecific", void *(*)(uint32_t));

    assert(bionic_key_create(NULL, NULL) == EINVAL);
    assert(bionic_key_delete(0) == EINVAL);
    assert(bionic_setspecific(0, NULL) == EINVAL);
    assert(bionic_getspecific(0) == NULL);

    for (unsigned i = 0; i < KEY_COUNT; i++) {
        assert(bionic_key_create(&keys[i], i == 0 ? destructor_rearm : NULL) == 0);
        assert(keys[i] == (0x80000000u | i));
        assert(bionic_getspecific(keys[i]) == NULL);
    }
    uint32_t excess = 0;
    assert(bionic_key_create(&excess, NULL) == EAGAIN);

    // All 128 Bionic keys resolve through a single host pthread TLS slot.
    for (unsigned i = 1; i < KEY_COUNT; i++) {
        assert(bionic_setspecific(keys[i], (void *)(uintptr_t)(i + 1)) == 0);
        assert((uintptr_t)bionic_getspecific(keys[i]) == i + 1);
    }

    pthread_t threads[WORKER_COUNT];
    for (unsigned i = 0; i < WORKER_COUNT; i++)
        assert(pthread_create(&threads[i], NULL, worker, (void *)(uintptr_t)i) == 0);
    for (unsigned i = 0; i < WORKER_COUNT; i++)
        assert(pthread_join(threads[i], NULL) == 0);
    assert(destroyed_values == WORKER_COUNT * 2);
    assert(cleared_before_destructor == destroyed_values);
    assert(bionic_getspecific(keys[0]) == NULL);
    assert((uintptr_t)bionic_getspecific(keys[1]) == 2);

    // Reusing a numeric Bionic key must not return stale per-thread values.
    uint32_t former = keys[1];
    assert(bionic_key_delete(former) == 0);
    assert(bionic_getspecific(former) == NULL);
    assert(bionic_setspecific(former, (void *)1) == EINVAL);
    assert(bionic_key_create(&keys[1], NULL) == 0);
    assert(keys[1] == former);
    assert(bionic_getspecific(keys[1]) == NULL);
    assert(bionic_setspecific(keys[1], (void *)(uintptr_t)500) == 0);
    assert((uintptr_t)bionic_getspecific(keys[1]) == 500);

    for (unsigned i = 0; i < KEY_COUNT; i++)
        assert(bionic_key_delete(keys[i]) == 0);
    assert(bionic_key_delete(keys[1]) == EINVAL);
    puts("android_tls_compat_test: 128 Bionic keys, generations and destructors passed");
    return 0;
}
