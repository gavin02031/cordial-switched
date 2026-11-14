#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "android_cxa_compat.h"

typedef int (*Register)(void (*)(void *), void *, void *);
typedef void (*Finalize)(void *);

static int sequence[16];
static unsigned count;
static Register register_handler;
static Finalize finalize_handler;
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

int main(void)
{
    uint64_t registration = 0, finalization = 0, address = 0;
    assert(android_cxa_compat_resolve("__cxa_atexit", "LIBC", "libc.so", &registration));
    assert(android_cxa_compat_resolve("__cxa_finalize", "LIBC", "libc.so", &finalization));
    assert(registration && finalization);
    assert(!android_cxa_compat_resolve("__cxa_atexit", "LIBC", "libdl.so", &address));
    assert(!android_cxa_compat_resolve("__cxa_atexit", "WRONG", "libc.so", &address));
    assert(!android_cxa_compat_resolve("__register_atfork", "LIBC", "libc.so", &address));
    memcpy(&register_handler, &registration, sizeof(registration));
    memcpy(&finalize_handler, &finalization, sizeof(finalization));
    assert(register_handler(NULL, NULL, &module_a) == -1);

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
    puts("android_cxa_compat_test: all assertions passed");
}
