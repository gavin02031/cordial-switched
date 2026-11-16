#include "android_cxa_compat.h"

#include <errno.h>
#include <pthread.h>
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

typedef struct CxaThreadHandlers {
    CxaExitHandler *pending;
    unsigned count;
} CxaThreadHandlers;

static pthread_once_t thread_key_once = PTHREAD_ONCE_INIT;
static pthread_key_t thread_key;
static int thread_key_error;

static void destroy_thread_handlers(void *opaque)
{
    CxaThreadHandlers *handlers = opaque;
    // Keep the key bound while destructors run: a C++ thread-local destructor
    // may initialise another thread-local object. Its new callback must run
    // before older pending callbacks, not disappear at thread exit.
    (void)pthread_setspecific(thread_key, handlers);
    while (handlers->pending) {
        CxaExitHandler *entry = handlers->pending;
        handlers->pending = entry->next;
        handlers->count--;
        void (*destroy)(void *) = entry->destroy;
        void *argument = entry->argument;
        free(entry);
        destroy(argument);
    }
    (void)pthread_setspecific(thread_key, NULL);
    free(handlers);
}

static void make_thread_key(void)
{
    thread_key_error = pthread_key_create(&thread_key, destroy_thread_handlers);
}

static int bionic_cxa_thread_atexit_impl(void (*destroy)(void *),
                                        void *argument, void *dso)
{
    if (!destroy) {
        errno = EINVAL;
        return -1;
    }
    int error = pthread_once(&thread_key_once, make_thread_key);
    if (!error)
        error = thread_key_error;
    if (error) {
        errno = error;
        return -1;
    }

    CxaThreadHandlers *handlers = pthread_getspecific(thread_key);
    bool new_handlers = handlers == NULL;
    if (new_handlers) {
        handlers = calloc(1, sizeof(*handlers));
        if (!handlers) {
            errno = ENOMEM;
            return -1;
        }
    }
    if (handlers->count >= ANDROID_CXA_MAX_PENDING) {
        if (new_handlers) free(handlers);
        errno = ENOMEM;
        return -1;
    }
    CxaExitHandler *entry = malloc(sizeof(*entry));
    if (!entry) {
        if (new_handlers) free(handlers);
        errno = ENOMEM;
        return -1;
    }
    entry->destroy = destroy;
    entry->argument = argument;
    entry->dso = dso;
    if (new_handlers) {
        error = pthread_setspecific(thread_key, handlers);
        if (error) {
            free(entry);
            free(handlers);
            errno = error;
            return -1;
        }
    }
    entry->next = handlers->pending;
    handlers->pending = entry;
    handlers->count++;
    return 0;
}

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
    else if (strcmp(name, "__cxa_thread_atexit_impl") == 0)
        *address = (uint64_t)(uintptr_t)&bionic_cxa_thread_atexit_impl;
    return *address != 0;
}
