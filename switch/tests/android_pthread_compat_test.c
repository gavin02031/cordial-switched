#define _POSIX_C_SOURCE 200809L
#include "android_pthread_compat.h"

#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include <stdlib.h>

// Deliberately model Android LP64 storage, never host pthread structures.
typedef struct { int32_t private_words[10]; } BionicMutex;
typedef struct { int32_t private_words[12]; } BionicCond;
typedef struct { int32_t private_words[14]; } BionicRwlock;

static int (*bridge_lock)(void *);
static int (*bridge_trylock)(void *);
static int (*bridge_unlock)(void *);
static int (*bridge_destroy)(void *);
static int (*bridge_wait)(void *, void *);
static int (*bridge_signal)(void *);
static int (*bridge_broadcast)(void *);
static int (*bridge_once)(volatile int *, void (*)(void));
static int (*bridge_rw_rdlock)(void *);
static int (*bridge_rw_wrlock)(void *);
static int (*bridge_rw_unlock)(void *);

static BionicMutex shared_mutex = {{0}};
static BionicCond shared_cond = {{0}};
static unsigned shared_count;
static int shared_ready;
static int once_calls;
static volatile int once_control;
static BionicRwlock shared_rwlock = {{0}};
static unsigned rw_left;
static unsigned rw_right;

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

static void *rw_reader_worker(void *unused)
{
    (void)unused;
    for (unsigned i = 0; i < 1500; i++) {
        assert(bridge_rw_rdlock(&shared_rwlock) == 0);
        assert(rw_left == rw_right);
        assert(bridge_rw_unlock(&shared_rwlock) == 0);
    }
    return NULL;
}

static void *rw_writer_worker(void *unused)
{
    (void)unused;
    for (unsigned i = 0; i < 1500; i++) {
        assert(bridge_rw_wrlock(&shared_rwlock) == 0);
        ++rw_left;
        rw_right = rw_left;
        assert(bridge_rw_unlock(&shared_rwlock) == 0);
    }
    return NULL;
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
    assert(!android_pthread_compat_resolve("pthread_rwlock_rdlock", "LIBC_N", "libc.so", &address));
    assert(!android_pthread_compat_resolve("pthread_mutex_lock", "LIBC", "libc.so", NULL));

    bridge_lock = RESOLVE("pthread_mutex_lock", int (*)(void *));
    bridge_trylock = RESOLVE("pthread_mutex_trylock", int (*)(void *));
    bridge_unlock = RESOLVE("pthread_mutex_unlock", int (*)(void *));
    bridge_destroy = RESOLVE("pthread_mutex_destroy", int (*)(void *));
    bridge_wait = RESOLVE("pthread_cond_wait", int (*)(void *, void *));
    bridge_signal = RESOLVE("pthread_cond_signal", int (*)(void *));
    bridge_broadcast = RESOLVE("pthread_cond_broadcast", int (*)(void *));
    bridge_once = RESOLVE("pthread_once", int (*)(volatile int *, void (*)(void)));
    bridge_rw_rdlock = RESOLVE("pthread_rwlock_rdlock", int (*)(void *));
    bridge_rw_wrlock = RESOLVE("pthread_rwlock_wrlock", int (*)(void *));
    bridge_rw_unlock = RESOLVE("pthread_rwlock_unlock", int (*)(void *));
    int (*rw_init)(void *, const void *) = RESOLVE("pthread_rwlock_init", int (*)(void *, const void *));
    int (*rw_destroy)(void *) = RESOLVE("pthread_rwlock_destroy", int (*)(void *));
    int (*cond_timedwait)(void *, void *, const struct timespec *) =
        RESOLVE("pthread_cond_timedwait", int (*)(void *, void *, const struct timespec *));
    int (*condattr_init)(void *) = RESOLVE("pthread_condattr_init", int (*)(void *));
    int (*condattr_clock)(void *, int) = RESOLVE("pthread_condattr_setclock", int (*)(void *, int));
    int (*condattr_destroy)(void *) = RESOLVE("pthread_condattr_destroy", int (*)(void *));
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

    BionicMutex timed_mutex = {{0}};
    BionicCond timed_cond = {{0}};
    assert(bridge_lock(&timed_mutex) == 0);
    struct timespec expired = {0, 1};
    assert(cond_timedwait(&timed_cond, &timed_mutex, &expired) == ETIMEDOUT);
    assert(bridge_unlock(&timed_mutex) == 0);
    assert(cond_timedwait(&timed_cond, &timed_mutex, NULL) == EINVAL);
    expired.tv_nsec = -1;
    assert(cond_timedwait(&timed_cond, &timed_mutex, &expired) == EINVAL);
    assert(cond_destroy(&timed_cond) == 0);
    assert(bridge_destroy(&timed_mutex) == 0);
    long clock_attr = -1;
    assert(condattr_init(&clock_attr) == 0 && clock_attr == 0);
    assert(condattr_clock(&clock_attr, 1) == 0 && clock_attr == 2);
    assert(condattr_clock(&clock_attr, 78) == EINVAL);
    assert(clock_attr == 2);
    assert(condattr_clock(&clock_attr, 0) == 0 && clock_attr == 0);
    assert(condattr_destroy(&clock_attr) == 0);

    // A future monotonic deadline interpreted as realtime expires at once.
    // Exercise both the explicit attributes and Bionic's static marker.
    for (unsigned mode = 0; mode < 3; ++mode) {
        BionicCond clock_cond = {{0}};
        BionicMutex clock_mutex = {{0}};
        clockid_t clock = mode == 2 ? CLOCK_REALTIME : CLOCK_MONOTONIC;
        if (mode == 0) {
            assert(condattr_init(&clock_attr) == 0);
            assert(condattr_clock(&clock_attr, 1) == 0);
            assert(cond_init(&clock_cond, &clock_attr) == 0);
            // Changing the attr must not change an already-created object.
            assert(condattr_clock(&clock_attr, 0) == 0);
            assert(condattr_destroy(&clock_attr) == 0);
        } else if (mode == 1) clock_cond.private_words[0] = 2;
        struct timespec start, finish, deadline;
        assert(clock_gettime(CLOCK_MONOTONIC, &start) == 0);
        assert(clock_gettime(clock, &deadline) == 0);
        deadline.tv_nsec += 30000000;
        if (deadline.tv_nsec >= 1000000000) {
            deadline.tv_sec++;
            deadline.tv_nsec -= 1000000000;
        }
        assert(bridge_lock(&clock_mutex) == 0);
        int result;
        do {
            result = cond_timedwait(&clock_cond, &clock_mutex, &deadline);
        } while (result == 0); // POSIX permits spurious wakeups.
        assert(result == ETIMEDOUT);
        assert(clock_gettime(CLOCK_MONOTONIC, &finish) == 0);
        int64_t elapsed = (finish.tv_sec - start.tv_sec) * INT64_C(1000000000)
                          + finish.tv_nsec - start.tv_nsec;
        assert(elapsed >= 20000000);
        assert(bridge_unlock(&clock_mutex) == 0);
        assert(cond_destroy(&clock_cond) == 0);
        assert(bridge_destroy(&clock_mutex) == 0);
    }
    // A real signal must wake a timed monotonic waiter and reacquire its mutex.
    assert(condattr_init(&clock_attr) == 0);
    assert(condattr_clock(&clock_attr, 1) == 0);
    assert(cond_init(&shared_cond, &clock_attr) == 0);
    assert(condattr_destroy(&clock_attr) == 0);
    assert(bridge_lock(&shared_mutex) == 0);
    shared_ready = 0;
    pthread_t monotonic_signaler;
    assert(pthread_create(&monotonic_signaler, NULL, signal_worker, NULL) == 0);
    struct timespec wake_deadline;
    assert(clock_gettime(CLOCK_MONOTONIC, &wake_deadline) == 0);
    wake_deadline.tv_sec += 5;
    while (!shared_ready)
        assert(cond_timedwait(&shared_cond, &shared_mutex, &wake_deadline) == 0);
    assert(bridge_unlock(&shared_mutex) == 0);
    assert(pthread_join(monotonic_signaler, NULL) == 0);
    assert(cond_destroy(&shared_cond) == 0);
    assert(bridge_destroy(&shared_mutex) == 0);
    BionicCond shared_clock_cond = {{3}};
    assert(bridge_signal(&shared_clock_cond) == ENOTSUP);
    clock_attr = 3;
    assert(cond_init(&shared_clock_cond, &clock_attr) == ENOTSUP);
    assert(condattr_clock(&clock_attr, 1) == ENOTSUP && clock_attr == 3);

    assert(bridge_rw_rdlock(&shared_rwlock) == 0);
    assert(bridge_rw_rdlock(&shared_rwlock) == 0);
    assert(bridge_rw_wrlock(&shared_rwlock) == EDEADLK);
    assert(rw_destroy(&shared_rwlock) == EBUSY);
    assert(bridge_rw_unlock(&shared_rwlock) == 0);
    assert(bridge_rw_unlock(&shared_rwlock) == 0);
    pthread_t rw_threads[7];
    assert(pthread_create(&rw_threads[0], NULL, rw_writer_worker, NULL) == 0);
    for (unsigned i = 1; i < 7; ++i)
        assert(pthread_create(&rw_threads[i], NULL, rw_reader_worker, NULL) == 0);
    for (unsigned i = 0; i < 7; ++i)
        assert(pthread_join(rw_threads[i], NULL) == 0);
    assert(rw_left == 1500 && rw_right == rw_left);
    assert(bridge_rw_wrlock(&shared_rwlock) == 0);
    assert(bridge_rw_wrlock(&shared_rwlock) == EDEADLK);
    assert(bridge_rw_rdlock(&shared_rwlock) == EDEADLK);
    assert(bridge_rw_unlock(&shared_rwlock) == 0);
    assert(bridge_rw_unlock(&shared_rwlock) == EPERM);
    assert(rw_destroy(&shared_rwlock) == 0);
    assert(rw_destroy(NULL) == EINVAL);
    BionicRwlock explicit_rw = {{0}};
    long rw_attr = 12;
    assert(rw_init(&explicit_rw, &rw_attr) == ENOTSUP);
    assert(rw_init(&explicit_rw, NULL) == 0);
    assert(rw_init(&explicit_rw, NULL) == EBUSY);
    assert(rw_destroy(&explicit_rw) == 0);

    BionicCond explicit_cond = {{0}};
    long unsupported_attr = 0x100;
    assert(cond_init(&explicit_cond, &unsupported_attr) == ENOTSUP);
    assert(cond_init(&explicit_cond, NULL) == 0);
    assert(cond_destroy(&explicit_cond) == 0);

    // Keep enough live objects to exercise bucket collisions and removal
    // from chains. Destroy in a different order, then reuse the same storage.
    enum { MANY_LOCKS = 4096 };
    BionicMutex *many = calloc(MANY_LOCKS, sizeof(*many));
    assert(many);
    for (unsigned i = 0; i < MANY_LOCKS; ++i) {
        assert(bridge_lock(&many[i]) == 0);
        assert(bridge_unlock(&many[i]) == 0);
    }
    for (unsigned i = 0; i < MANY_LOCKS; ++i) {
        unsigned slot = (i * 2053u) % MANY_LOCKS;
        assert(bridge_lock(&many[slot]) == 0);
        assert(bridge_destroy(&many[slot]) == EBUSY);
        assert(bridge_unlock(&many[slot]) == 0);
        assert(bridge_destroy(&many[slot]) == 0);
    }
    for (unsigned i = 0; i < MANY_LOCKS; ++i) {
        assert(mutex_init(&many[i], NULL) == 0);
        assert(bridge_lock(&many[i]) == 0);
        assert(bridge_unlock(&many[i]) == 0);
        assert(bridge_destroy(&many[i]) == 0);
    }
    free(many);

    puts("android_pthread_compat_test: Bionic mutex, cond and once ABI bridge passed");
    return 0;
}
