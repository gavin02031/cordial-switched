#define _POSIX_C_SOURCE 200809L
#include "android_thread_lifecycle_compat.h"
#include "android_thread_attr_compat.h"

#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

typedef struct {
    uint32_t flags;
    void *stack_base;
    size_t stack_size;
    size_t guard_size;
    int32_t sched_policy;
    int32_t sched_priority;
    char reserved[16];
} AndroidAttr;

_Static_assert(sizeof(AndroidAttr) == 56, "Bionic LP64 thread attr size");

static uint64_t resolve(const char *name)
{
    uint64_t address = 0;
    assert(android_thread_lifecycle_compat_resolve(name, "LIBC", "libc.so", &address));
    return address;
}

#define BIND(name, type) ((type)(uintptr_t)resolve(name))

static uint64_t (*bionic_self)(void);
static atomic_int detached_done;

typedef struct {
    uint64_t identity;
    void *result;
    const void *const *shadow;
} Worker;

static void *worker_start(void *arg)
{
    Worker *worker = arg;
    worker->identity = bionic_self();
    worker->shadow = android_thread_lifecycle_compat_shadow_current();
    assert(worker->shadow);
    assert(worker->shadow[-1] == NULL); // No Bionic TLS allocation exists.
    assert(worker->shadow[0] == NULL);  // No ELF DTV has been constructed.
    assert(worker->shadow[1] == (void *)(uintptr_t)worker->identity);
    assert(worker->shadow[5] == NULL); // No stack guard has been installed.
    return worker->result;
}

static void *detached_start(void *arg)
{
    (void)arg;
    assert(bionic_self() != 0);
    atomic_fetch_add(&detached_done, 1);
    return NULL;
}

int main(void)
{
    int (*create)(uint64_t *, const void *, void *(*)(void *), void *) =
        BIND("pthread_create", int (*)(uint64_t *, const void *, void *(*)(void *), void *));
    int (*join)(uint64_t, void **) = BIND("pthread_join", int (*)(uint64_t, void **));
    int (*detach)(uint64_t) = BIND("pthread_detach", int (*)(uint64_t));
    int (*equal)(uint64_t, uint64_t) = BIND("pthread_equal", int (*)(uint64_t, uint64_t));
    bionic_self = BIND("pthread_self", uint64_t (*)(void));
    uint64_t rejected = 0;
    assert(!android_thread_lifecycle_compat_resolve("pthread_create", "LIBM", "libc.so", &rejected));
    assert(!android_thread_lifecycle_compat_resolve("pthread_create", "LIBC", "libm.so", &rejected));
    assert(!android_thread_lifecycle_compat_resolve("pthread_exit", "LIBC", "libc.so", &rejected));
    assert(!android_thread_lifecycle_compat_resolve("pthread_create", "LIBC", "libc.so", NULL));

    uint64_t main_id = bionic_self();
    assert(main_id && main_id == bionic_self());
    const void *const *main_shadow = android_thread_lifecycle_compat_shadow_current();
    assert(main_shadow && main_shadow[-1] == NULL);
    assert(main_shadow[1] == (void *)(uintptr_t)main_id);
    assert(equal(main_id, main_id) == 1);
    assert(join(main_id, NULL) == EINVAL);
    assert(detach(main_id) == EINVAL);
    assert(join(UINT64_MAX, NULL) == EINVAL);
    assert(detach(UINT64_MAX) == EINVAL);
    assert(create(NULL, NULL, worker_start, NULL) == EINVAL);
    assert(create(&rejected, NULL, NULL, NULL) == EINVAL);

    enum { WORKERS = 64 };
    Worker workers[WORKERS];
    uint64_t threads[WORKERS];
    for (unsigned i = 0; i < WORKERS; i++) {
        workers[i] = (Worker){.identity = 0, .result = (void *)(uintptr_t)(i + 1)};
        assert(create(&threads[i], NULL, worker_start, &workers[i]) == 0);
        assert(threads[i] && !equal(threads[i], main_id));
    }
    for (unsigned i = 0; i < WORKERS; i++) {
        void *result = NULL;
        assert(join(threads[i], &result) == 0);
        assert(result == workers[i].result);
        assert(workers[i].identity == threads[i]);
        assert(workers[i].shadow != main_shadow);
        for (unsigned j = 0; j < i; j++)
            assert(workers[i].shadow != workers[j].shadow);
        assert(join(threads[i], NULL) == EINVAL);
        assert(detach(threads[i]) == EINVAL);
    }

    uint64_t address = 0;
    assert(android_thread_attr_compat_resolve("pthread_attr_init", "LIBC", "libc.so", &address));
    int (*init)(void *) = (int (*)(void *))(uintptr_t)address;
    assert(android_thread_attr_compat_resolve("pthread_attr_setdetachstate", "LIBC", "libc.so", &address));
    int (*setdetach)(void *, int) = (int (*)(void *, int))(uintptr_t)address;
    AndroidAttr attr;
    assert(init(&attr) == 0);
    assert(setdetach(&attr, 1) == 0);
    uint64_t token = 0;
    atomic_init(&detached_done, 0);
    assert(create(&token, &attr, detached_start, NULL) == 0);
    assert(token);
    assert(join(token, NULL) == EINVAL);
    assert(detach(token) == EINVAL);

    struct timespec wait_time = {0, 1000000};
    for (unsigned i = 0; i < 500 && !atomic_load(&detached_done); i++)
        nanosleep(&wait_time, NULL);
    assert(atomic_load(&detached_done) == 1);

    // Invalid or unimplemented Bionic attribute features must fail before a
    // native thread can be created; no partial handle is exposed to callers.
    assert(init(&attr) == 0);
    attr.sched_priority = 7;
    assert(create(&token, &attr, worker_start, NULL) == ENOTSUP);
    assert(init(&attr) == 0);
    attr.stack_base = (void *)(uintptr_t)0x10000;
    assert(create(&token, &attr, worker_start, NULL) == ENOTSUP);

    printf("android_thread_lifecycle_compat_test: %u worker joins, detached and invalid-handle checks passed\n",
           WORKERS);
    return 0;
}
