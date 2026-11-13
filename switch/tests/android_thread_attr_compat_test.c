#include "android_thread_attr_compat.h"

#include <assert.h>
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    uint32_t flags;
    void *stack_base;
    size_t stack_size;
    size_t guard_size;
    int32_t sched_policy;
    int32_t sched_priority;
    char reserved[16];
} AndroidAttr;

_Static_assert(sizeof(AndroidAttr) == 56, "Android pthread_attr_t layout changed");

static uint64_t lookup(const char *name)
{
    uint64_t address = 0;
    assert(android_thread_attr_compat_resolve(name, "LIBC", "libc.so", &address));
    return address;
}

#define RESOLVE(name, type) ((type)(uintptr_t)lookup(name))

int main(void)
{
    uint64_t address = 0;
    assert(!android_thread_attr_compat_resolve("pthread_attr_init", "LIBM", "libc.so", &address));
    assert(!android_thread_attr_compat_resolve("pthread_attr_init", "LIBC", "libm.so", &address));
    assert(!android_thread_attr_compat_resolve("pthread_create", "LIBC", "libc.so", &address));
    assert(!android_thread_attr_compat_resolve("pthread_attr_setstack", "LIBC", "libc.so", &address));
    assert(!android_thread_attr_compat_resolve("pthread_attr_init", "LIBC", "libc.so", NULL));

    int (*init)(void *) = RESOLVE("pthread_attr_init", int (*)(void *));
    int (*destroy)(void *) = RESOLVE("pthread_attr_destroy", int (*)(void *));
    int (*setdetach)(void *, int) = RESOLVE("pthread_attr_setdetachstate", int (*)(void *, int));
    int (*getdetach)(const void *, int *) = RESOLVE("pthread_attr_getdetachstate", int (*)(const void *, int *));
    int (*setstack)(void *, size_t) = RESOLVE("pthread_attr_setstacksize", int (*)(void *, size_t));
    int (*getstack)(const void *, size_t *) = RESOLVE("pthread_attr_getstacksize", int (*)(const void *, size_t *));
    int (*setguard)(void *, size_t) = RESOLVE("pthread_attr_setguardsize", int (*)(void *, size_t));
    int (*getguard)(const void *, size_t *) = RESOLVE("pthread_attr_getguardsize", int (*)(const void *, size_t *));
    int (*getstackaddr)(const void *, void **, size_t *) = RESOLVE("pthread_attr_getstack", int (*)(const void *, void **, size_t *));

    assert(init(NULL) == EINVAL);
    assert(destroy(NULL) == EINVAL);
    struct {
        uint64_t before;
        AndroidAttr value;
        uint64_t after;
    } frame;
    memset(&frame, 0xa5, sizeof(frame));
    frame.before = UINT64_C(0x123456789abcdef0);
    frame.after = UINT64_C(0xfedcba9876543210);
    assert(init(&frame.value) == 0);
    assert(frame.before == UINT64_C(0x123456789abcdef0));
    assert(frame.after == UINT64_C(0xfedcba9876543210));
    assert(frame.value.stack_base == NULL);
    assert(frame.value.stack_size == 1024u * 1024u - 32u * 1024u);
    assert(frame.value.guard_size == 16384);
    assert(frame.value.sched_policy == 0 && frame.value.sched_priority == 0);

    int detach = -1;
    assert(getdetach(&frame.value, &detach) == 0 && detach == 0);
    assert(setdetach(&frame.value, 1) == 0);
    assert(getdetach(&frame.value, &detach) == 0 && detach == 1);
    assert(setdetach(&frame.value, 2) == EINVAL);
    assert(setdetach(&frame.value, 0) == 0);
    assert(getdetach(&frame.value, &detach) == 0 && detach == 0);

    size_t size = 0;
    assert(setstack(&frame.value, 16383) == EINVAL);
    assert(getstack(&frame.value, &size) == 0 && size == 1024u * 1024u - 32u * 1024u);
    assert(setstack(&frame.value, 2u * 1024u * 1024u) == 0);
    assert(getstack(&frame.value, &size) == 0 && size == 2u * 1024u * 1024u);
    assert(setguard(&frame.value, 32768) == 0);
    assert(getguard(&frame.value, &size) == 0 && size == 32768);
    void *stack_base = (void *)1;
    assert(getstackaddr(&frame.value, &stack_base, &size) == 0);
    assert(stack_base == NULL && size == 2u * 1024u * 1024u);
    assert(getstackaddr(&frame.value, NULL, &size) == EINVAL);

    assert(destroy(&frame.value) == 0);
    assert(frame.before == UINT64_C(0x123456789abcdef0));
    assert(frame.after == UINT64_C(0xfedcba9876543210));
    assert(setstack(&frame.value, 65536) == EINVAL);
    assert(getdetach(&frame.value, &detach) == EINVAL);
    assert(init(&frame.value) == 0);

    puts("android_thread_attr_compat_test: Bionic LP64 attr layout and bounds passed");
    return 0;
}
