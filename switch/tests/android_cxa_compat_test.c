#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "android_cxa_compat.h"

typedef int (*Register)(void (*)(void *), void *, void *);
typedef void (*Finalize)(void *);
typedef int (*RegisterThread)(void (*)(void *), void *, void *);

static int sequence[16];
static unsigned count;
static Register register_handler;
static Finalize finalize_handler;
static RegisterThread register_thread_handler;
static int module_a;
static int module_b;

static void record(void *value)
{
    assert(count < 16);
    sequence[count++] = (int)(intptr_t)value;
}

static void register_during_finalize(void *value)
{
    record(value);
    assert(register_handler(record, (void *)(intptr_t)4, &module_a) == 0);
    // Reentrant finalization must not invoke the current callback twice.
    finalize_handler(&module_b);
}

static void register_during_thread_exit(void *value)
{
    record(value);
    assert(register_thread_handler(record, (void *)(intptr_t)9, &module_a) == 0);
}

static void *thread_local_worker(void *opaque)
{
    int base = (int)(intptr_t)opaque;
    assert(register_thread_handler(record, (void *)(intptr_t)base, &module_a) == 0);
    assert(register_thread_handler(record, (void *)(intptr_t)(base + 1), &module_b) == 0);
    assert(register_thread_handler(register_during_thread_exit,
                                   (void *)(intptr_t)(base + 2), &module_a) == 0);
    return NULL;
}

int main(void)
{
    uint64_t registration = 0, finalization = 0, thread_registration = 0, address = 0;
    assert(android_cxa_compat_resolve("__cxa_atexit", "LIBC", "libc.so", &registration));
    assert(android_cxa_compat_resolve("__cxa_finalize", "LIBC", "libc.so", &finalization));
    assert(android_cxa_compat_resolve("__cxa_thread_atexit_impl", "LIBC", "libc.so",
                                      &thread_registration));
    assert(registration && finalization && thread_registration);
    assert(!android_cxa_compat_resolve("__cxa_atexit", "LIBC", "libdl.so", &address));
    assert(!android_cxa_compat_resolve("__cxa_atexit", "WRONG", "libc.so", &address));
    assert(!android_cxa_compat_resolve("__register_atfork", "LIBC", "libc.so", &address));
    assert(!android_cxa_compat_resolve("__cxa_thread_atexit_impl", "LIBC_N", "libc.so",
                                       &address));
    memcpy(&register_handler, &registration, sizeof(registration));
    memcpy(&finalize_handler, &finalization, sizeof(finalization));
    memcpy(&register_thread_handler, &thread_registration, sizeof(thread_registration));
    assert(register_handler(NULL, NULL, &module_a) == -1);
    assert(register_thread_handler(NULL, NULL, &module_a) == -1);

    assert(register_handler(record, (void *)(intptr_t)1, &module_a) == 0);
    assert(register_handler(record, (void *)(intptr_t)2, &module_b) == 0);
    assert(register_handler(register_during_finalize, (void *)(intptr_t)3, &module_a) == 0);
    finalize_handler(&module_a);
    assert(count == 4);
    assert(sequence[0] == 3 && sequence[1] == 2 && sequence[2] == 4 && sequence[3] == 1);
    finalize_handler(&module_a);
    finalize_handler(&module_b);
    assert(count == 4); // Exactly once per registered destructor.

    assert(register_handler(record, (void *)(intptr_t)5, &module_b) == 0);
    assert(register_handler(record, (void *)(intptr_t)6, &module_a) == 0);
    finalize_handler(NULL); // Global C++ shutdown: reverse registration order.
    assert(count == 6 && sequence[4] == 6 && sequence[5] == 5);
    finalize_handler(NULL);
    assert(count == 6);

    // Native thread exit must invoke the *thread's* pending handlers in LIFO
    // order, including a callback registered from within another destructor.
    pthread_t thread;
    assert(pthread_create(&thread, NULL, thread_local_worker,
                          (void *)(intptr_t)10) == 0);
    assert(pthread_join(thread, NULL) == 0);
    assert(count == 10);
    assert(sequence[6] == 12 && sequence[7] == 9);
    assert(sequence[8] == 11 && sequence[9] == 10);

    assert(pthread_create(&thread, NULL, thread_local_worker,
                          (void *)(intptr_t)20) == 0);
    assert(pthread_join(thread, NULL) == 0);
    assert(count == 14);
    assert(sequence[10] == 22 && sequence[11] == 9);
    assert(sequence[12] == 21 && sequence[13] == 20);
    finalize_handler(NULL); // Global finalisation must not re-run TLS callbacks.
    assert(count == 14);
    puts("android_cxa_compat_test: all assertions passed");
}
