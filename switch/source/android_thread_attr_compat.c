#include "android_thread_attr_compat.h"

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

// The exact Bionic LP64 pthread_attr_t layout, independent of libnx/newlib.
// AOSP: platform/bionic/libc/include/bits/pthread_types.h
typedef struct {
    uint32_t flags;
    void *stack_base;
    size_t stack_size;
    size_t guard_size;
    int32_t sched_policy;
    int32_t sched_priority;
    char reserved[16];
} AndroidPthreadAttr;

_Static_assert(sizeof(AndroidPthreadAttr) == 56, "unexpected Bionic pthread_attr_t size");
_Static_assert(offsetof(AndroidPthreadAttr, stack_base) == 8, "unexpected Bionic stack_base offset");
_Static_assert(offsetof(AndroidPthreadAttr, stack_size) == 16, "unexpected Bionic stack_size offset");
_Static_assert(offsetof(AndroidPthreadAttr, guard_size) == 24, "unexpected Bionic guard_size offset");
_Static_assert(offsetof(AndroidPthreadAttr, sched_policy) == 32, "unexpected Bionic sched_policy offset");

enum {
    ANDROID_ATTR_DETACHED = 1,
    ANDROID_STACK_MINIMUM = 16384,
    ANDROID_DEFAULT_STACK = 1024 * 1024 - 32 * 1024,
    ANDROID_DEFAULT_GUARD = 16384,
};

static bool valid(const AndroidPthreadAttr *attr)
{
    // We own only the normal Android detach flag. Other flags may request
    // custom-stack or scheduling semantics we haven't implemented yet.
    return attr && (attr->flags & ~((uint32_t)ANDROID_ATTR_DETACHED)) == 0;
}

static int android_attr_init(void *address)
{
    if (!address) return EINVAL;
    AndroidPthreadAttr *attr = address;
    memset(attr, 0, sizeof(*attr));
    attr->stack_size = ANDROID_DEFAULT_STACK;
    attr->guard_size = ANDROID_DEFAULT_GUARD;
    return 0;
}

static int android_attr_destroy(void *address)
{
    if (!address) return EINVAL;
    // Match Bionic's poisoned storage after destruction, so accidental reuse
    // is rejected by the wrappers instead of silently succeeding.
    memset(address, 0x42, sizeof(AndroidPthreadAttr));
    return 0;
}

static int android_attr_setdetachstate(void *address, int state)
{
    AndroidPthreadAttr *attr = address;
    if (!valid(attr)) return EINVAL;
    if (state == 0) attr->flags &= ~((uint32_t)ANDROID_ATTR_DETACHED);
    else if (state == 1) attr->flags |= ANDROID_ATTR_DETACHED;
    else return EINVAL;
    return 0;
}

static int android_attr_getdetachstate(const void *address, int *state)
{
    const AndroidPthreadAttr *attr = address;
    if (!state || !valid(attr)) return EINVAL;
    *state = (attr->flags & ANDROID_ATTR_DETACHED) ? 1 : 0;
    return 0;
}

static int android_attr_setstacksize(void *address, size_t stack_size)
{
    AndroidPthreadAttr *attr = address;
    if (!valid(attr)) return EINVAL;
    if (stack_size < ANDROID_STACK_MINIMUM) return EINVAL;
    attr->stack_size = stack_size;
    return 0;
}

static int android_attr_getstacksize(const void *address, size_t *stack_size)
{
    const AndroidPthreadAttr *attr = address;
    if (!stack_size || !valid(attr)) return EINVAL;
    *stack_size = attr->stack_size;
    return 0;
}

static int android_attr_setguardsize(void *address, size_t guard_size)
{
    AndroidPthreadAttr *attr = address;
    if (!valid(attr)) return EINVAL;
    attr->guard_size = guard_size;
    return 0;
}

static int android_attr_getguardsize(const void *address, size_t *guard_size)
{
    const AndroidPthreadAttr *attr = address;
    if (!guard_size || !valid(attr)) return EINVAL;
    *guard_size = attr->guard_size;
    return 0;
}

static int android_attr_getstack(const void *address, void **stack_base,
                                 size_t *stack_size)
{
    const AndroidPthreadAttr *attr = address;
    if (!stack_base || !stack_size || !valid(attr)) return EINVAL;
    *stack_base = attr->stack_base;
    *stack_size = attr->stack_size;
    return 0;
}

int android_thread_attr_compat_to_native(const void *address, pthread_attr_t *native)
{
    if (!address || !native)
        return EINVAL;
    const AndroidPthreadAttr *attr = address;
    // Bionic's user-owned stacks and non-default schedulers require further
    // runtime work. Handing them to the host pthread ABI would be unsafe.
    if (!valid(attr) || attr->stack_base || attr->stack_size < ANDROID_STACK_MINIMUM ||
        attr->sched_policy || attr->sched_priority)
        return ENOTSUP;
    for (size_t i = 0; i < sizeof(attr->reserved); i++) {
        if (attr->reserved[i])
            return ENOTSUP;
    }
    int result = pthread_attr_init(native);
    if (result)
        return result;
    result = pthread_attr_setdetachstate(native,
              (attr->flags & ANDROID_ATTR_DETACHED) ? PTHREAD_CREATE_DETACHED : PTHREAD_CREATE_JOINABLE);
    if (!result)
        result = pthread_attr_setstacksize(native, attr->stack_size);
    if (!result)
        result = pthread_attr_setguardsize(native, attr->guard_size);
    if (result)
        pthread_attr_destroy(native);
    return result;
}

bool android_thread_attr_compat_resolve(const char *name, const char *version,
                                        const char *library, uint64_t *address)
{
    if (!name || !version || !library || !address ||
        strcmp(version, "LIBC") != 0 || strcmp(library, "libc.so") != 0)
        return false;
    *address = 0;
#define MAP_ANDROID(suffix, function) \
    if (strcmp(name, "pthread_attr_" suffix) == 0) \
        *address = (uint64_t)(uintptr_t)&function; else
    MAP_ANDROID("init", android_attr_init)
    MAP_ANDROID("destroy", android_attr_destroy)
    MAP_ANDROID("setdetachstate", android_attr_setdetachstate)
    MAP_ANDROID("getdetachstate", android_attr_getdetachstate)
    MAP_ANDROID("setstacksize", android_attr_setstacksize)
    MAP_ANDROID("getstacksize", android_attr_getstacksize)
    MAP_ANDROID("setguardsize", android_attr_setguardsize)
    MAP_ANDROID("getguardsize", android_attr_getguardsize)
    MAP_ANDROID("getstack", android_attr_getstack)
    (void)0;
#undef MAP_ANDROID
    return *address != 0;
}
