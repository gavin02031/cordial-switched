#define _POSIX_C_SOURCE 200809L
#include "android_pthread_compat.h"

#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>

// Deliberately model Android LP64 storage, never host pthread structures.
typedef struct { int32_t private_words[10]; } BionicMutex;
typedef struct { int32_t private_words[12]; } BionicCond;

static int (*bridge_lock)(void *);
static int (*bridge_trylock)(void *);
static int (*bridge_unlock)(void *);
static int (*bridge_destroy)(void *);
static int (*bridge_wait)(void *, void *);
static int (*bridge_signal)(void *);
static int (*bridge_broadcast)(void *);
static int (*bridge_once)(volatile int *, void (*)(void));

static BionicMutex shared_mutex = {{0}};
static BionicCond shared_cond = {{0}};
static unsigned shared_count;
static int shared_ready;
static int once_calls;
static volatile int once_control;

static void once_callback(void)
{
    __atomic_add_fetch(&once_calls, 1, __ATOMIC_RELAXED);
}

static void *increment_worker(void *unused)
{
    (void)unused;
    for (unsigned i = 0; i < 2500; i++) {
        assert(bridge_lock(&shared_mutex) == 0);
        shared_count++;
        assert(bridge_unlock(&shared_mutex) == 0);
        assert(bridge_once(&once_control, once_callback) == 0);
    }
    return NULL;
}

static void *signal_worker(void *unused)
{
    (void)unused;
    assert(bridge_lock(&shared_mutex) == 0);
    shared_ready = 1;
    assert(bridge_signal(&shared_cond) == 0);
    assert(bridge_unlock(&shared_mutex) == 0);
    return NULL;
}

static void *trylock_worker(void *unused)
{
    (void)unused;
    return (void *)(uintptr_t)bridge_trylock(&shared_mutex);
}

#define RESOLVE(name, type) ((type)(uintptr_t)lookup(name))

static uint64_t lookup(const char *name)
{
    uint64_t address = 0;
    assert(android_pthread_compat_resolve(name, "LIBC", "libc.so", &address));
    assert(address);
    return address;
}

int main(void)
{
    uint64_t address = 0;
    assert(!android_pthread_compat_resolve("pthread_mutex_lock", "LIBM", "libc.so", &address));
    assert(!android_pthread_compat_resolve("pthread_mutex_lock", "LIBC", "libm.so", &address));
    assert(!android_pthread_compat_resolve("pthread_create", "LIBC", "libc.so", &address));
    assert(!android_pthread_compat_resolve("pthread_cond_timedwait", "LIBC", "libc.so", &address));
    assert(!android_pthread_compat_resolve("pthread_mutex_lock", "LIBC", "libc.so", NULL));

    bridge_lock = RESOLVE("pthread_mutex_lock", int (*)(void *));
    bridge_trylock = RESOLVE("pthread_mutex_trylock", int (*)(void *));
    bridge_unlock = RESOLVE("pthread_mutex_unlock", int (*)(void *));
    bridge_destroy = RESOLVE("pthread_mutex_destroy", int (*)(void *));
    bridge_wait = RESOLVE("pthread_cond_wait", int (*)(void *, void *));
    bridge_signal = RESOLVE("pthread_cond_signal", int (*)(void *));
    bridge_broadcast = RESOLVE("pthread_cond_broadcast", int (*)(void *));
    bridge_once = RESOLVE("pthread_once", int (*)(volatile int *, void (*)(void)));
    int (*cond_destroy)(void *) = RESOLVE("pthread_cond_destroy", int (*)(void *));
    int (*mutex_init)(void *, const void *) = RESOLVE("pthread_mutex_init", int (*)(void *, const void *));
    int (*cond_init)(void *, const void *) = RESOLVE("pthread_cond_init", int (*)(void *, const void *));
    int (*attr_init)(void *) = RESOLVE("pthread_mutexattr_init", int (*)(void *));
    int (*attr_type)(void *, int) = RESOLVE("pthread_mutexattr_settype", int (*)(void *, int));
    int (*attr_destroy)(void *) = RESOLVE("pthread_mutexattr_destroy", int (*)(void *));

    BionicMutex recursive = {{0}};
    long attr = -1;
    assert(attr_init(&attr) == 0 && attr == 0);
    assert(attr_type(&attr, 1) == 0 && attr == 1);
    assert(mutex_init(&recursive, &attr) == 0);
    assert(attr_destroy(&attr) == 0);
    assert(bridge_lock(&recursive) == 0);
    assert(bridge_lock(&recursive) == 0);
    assert(bridge_destroy(&recursive) == EBUSY);
    assert(bridge_unlock(&recursive) == 0);
    assert(bridge_unlock(&recursive) == 0);
    assert(bridge_destroy(&recursive) == 0);

    BionicMutex recursive_static = {{0x4000}};
    assert(bridge_lock(&recursive_static) == 0);
    assert(bridge_lock(&recursive_static) == 0);
    assert(bridge_unlock(&recursive_static) == 0);
    assert(bridge_unlock(&recursive_static) == 0);
    assert(bridge_destroy(&recursive_static) == 0);

    BionicMutex errorcheck = {{0x8000}};
    assert(bridge_lock(&errorcheck) == 0);
    assert(bridge_lock(&errorcheck) == EDEADLK);
    assert(bridge_unlock(&errorcheck) == 0);
    assert(bridge_destroy(&errorcheck) == 0);

    BionicMutex unsupported = {{0x1000}};
    assert(bridge_lock(&unsupported) == ENOTSUP);
    assert(mutex_init(&unsupported, NULL) == 0);
    assert(bridge_destroy(&unsupported) == 0);
    assert(bridge_lock(NULL) == EINVAL);

    pthread_t workers[6];
    for (unsigned i = 0; i < 6; i++)
        assert(pthread_create(&workers[i], NULL, increment_worker, NULL) == 0);
    for (unsigned i = 0; i < 6; i++)
        assert(pthread_join(workers[i], NULL) == 0);
    assert(shared_count == 15000);
    assert(once_calls == 1 && once_control == 2);

    assert(bridge_lock(&shared_mutex) == 0);
    pthread_t contender;
    assert(pthread_create(&contender, NULL, trylock_worker, NULL) == 0);
    void *try_result = NULL;
    assert(pthread_join(contender, &try_result) == 0);
    assert((uintptr_t)try_result == EBUSY);
    assert(bridge_unlock(&shared_mutex) == 0);

    for (unsigned i = 0; i < 10; i++) {
        shared_ready = 0;
        pthread_t signaler;
        assert(pthread_create(&signaler, NULL, signal_worker, NULL) == 0);
        assert(bridge_lock(&shared_mutex) == 0);
        while (!shared_ready)
            assert(bridge_wait(&shared_cond, &shared_mutex) == 0);
        assert(bridge_unlock(&shared_mutex) == 0);
        assert(pthread_join(signaler, NULL) == 0);
    }
    assert(bridge_broadcast(&shared_cond) == 0);
    assert(cond_destroy(&shared_cond) == 0);
    assert(bridge_destroy(&shared_mutex) == 0);

    BionicCond explicit_cond = {{0}};
    long unsupported_attr = 0x100;
    assert(cond_init(&explicit_cond, &unsupported_attr) == ENOTSUP);
    assert(cond_init(&explicit_cond, NULL) == 0);
    assert(cond_destroy(&explicit_cond) == 0);

    puts("android_pthread_compat_test: Bionic mutex, cond and once ABI bridge passed");
    return 0;
}
