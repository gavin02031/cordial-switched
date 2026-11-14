#include "android_cxa_compat.h"

#include <errno.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

enum { ANDROID_CXA_MAX_PENDING = 8192 };

typedef struct CxaExitHandler {
    struct CxaExitHandler *next;
    void (*destroy)(void *);
    void *argument;
    void *dso;
} CxaExitHandler;

static atomic_flag handlers_lock = ATOMIC_FLAG_INIT;
static CxaExitHandler *pending_handlers;
static unsigned pending_count;

static void acquire_lock(void)
{
    while (atomic_flag_test_and_set_explicit(&handlers_lock, memory_order_acquire)) {
        // A destructor is always invoked *outside* the lock. The critical
        // section contains pointer operations only and cannot call the guest.
    }
}

static void release_lock(void)
{
    atomic_flag_clear_explicit(&handlers_lock, memory_order_release);
}

static int bionic_cxa_atexit(void (*destroy)(void *), void *argument, void *dso)
{
    if (!destroy) {
        errno = EINVAL;
        return -1;
    }
    CxaExitHandler *entry = malloc(sizeof(*entry));
    if (!entry) {
        errno = ENOMEM;
        return -1;
    }
    entry->destroy = destroy;
    entry->argument = argument;
    entry->dso = dso;
    acquire_lock();
    if (pending_count >= ANDROID_CXA_MAX_PENDING) {
        release_lock();
        free(entry);
        errno = ENOMEM;
        return -1;
    }
    entry->next = pending_handlers;
    pending_handlers = entry;
    pending_count++;
    release_lock();
    return 0;
}

static void bionic_cxa_finalize(void *dso)
{
    // Remove and mark a callback before executing it: a destructor may call
    // __cxa_finalize recursively or register another handler. Avoid invoking
    // an engine-owned callback while holding a native runtime lock.
    for (;;) {
        acquire_lock();
        CxaExitHandler **slot = &pending_handlers;
        while (*slot && dso && (*slot)->dso != dso)
            slot = &(*slot)->next;
        CxaExitHandler *entry = *slot;
        if (entry) {
            *slot = entry->next;
            pending_count--;
        }
        release_lock();
        if (!entry)
            return;
        void (*destroy)(void *) = entry->destroy;
        void *argument = entry->argument;
        free(entry);
        destroy(argument);
    }
}

bool android_cxa_compat_resolve(const char *name, const char *version,
                                const char *library, uint64_t *address)
{
    if (!name || !version || !library || !address ||
        strcmp(version, "LIBC") != 0 || strcmp(library, "libc.so") != 0)
        return false;
    *address = 0;
    if (strcmp(name, "__cxa_atexit") == 0)
        *address = (uint64_t)(uintptr_t)&bionic_cxa_atexit;
    else if (strcmp(name, "__cxa_finalize") == 0)
        *address = (uint64_t)(uintptr_t)&bionic_cxa_finalize;
    return *address != 0;
}
