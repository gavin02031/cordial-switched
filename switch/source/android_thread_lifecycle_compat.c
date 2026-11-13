#define _POSIX_C_SOURCE 200809L
#include "android_thread_lifecycle_compat.h"

#include <errno.h>
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "android_thread_attr_compat.h"

// Bionic's pthread_t is a 64-bit pointer to its own thread structure, not a
// libnx/newlib pthread_t. Stable, non-reused registry addresses keep an old
// handle from ever referring to a newly created thread. This remains an ABI
// experiment: Bionic TLS, stack metadata and pthread_internal_t are absent.
enum { MAX_BRIDGED_THREADS = 4096 };

typedef struct {
    pthread_t native;
    void *(*start)(void *);
    void *argument;
    bool occupied;
    bool joinable;
    bool joining;
    bool consumed;
} ThreadEntry;

static ThreadEntry entries[MAX_BRIDGED_THREADS];
static unsigned entry_count;
static pthread_mutex_t registry_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_once_t identity_once = PTHREAD_ONCE_INIT;
static pthread_key_t identity_key;
static int identity_error;

static void initialize_identity(void)
{
    identity_error = pthread_key_create(&identity_key, NULL);
}

static int ensure_identity(void)
{
    int result = pthread_once(&identity_once, initialize_identity);
    return result ? result : identity_error;
}

static ThreadEntry *find_entry(uint64_t token)
{
    for (unsigned i = 0; i < entry_count; i++) {
        if (entries[i].occupied && (uint64_t)(uintptr_t)&entries[i] == token)
            return &entries[i];
    }
    return NULL;
}

static ThreadEntry *reserve_entry(void)
{
    if (entry_count >= MAX_BRIDGED_THREADS)
        return NULL;
    ThreadEntry *entry = &entries[entry_count++];
    entry->occupied = true;
    return entry;
}

static void *native_start(void *opaque)
{
    ThreadEntry *entry = opaque;
    // A failed native TLS registration must not call Android code under an
    // incorrect pthread_self identity.
    if (pthread_setspecific(identity_key, entry) != 0)
        return NULL;
    return entry->start(entry->argument);
}

static int bridge_create(uint64_t *token, const void *attr,
                         void *(*start)(void *), void *argument)
{
    if (!token || !start)
        return EINVAL;
    int result = ensure_identity();
    if (result)
        return result;
    pthread_attr_t native_attr;
    pthread_attr_t *translated = NULL;
    bool joinable = true;
    if (attr) {
        result = android_thread_attr_compat_to_native(attr, &native_attr);
        if (result)
            return result;
        translated = &native_attr;
        int detach_state;
        result = pthread_attr_getdetachstate(translated, &detach_state);
        if (result) {
            pthread_attr_destroy(translated);
            return result;
        }
        joinable = detach_state == PTHREAD_CREATE_JOINABLE;
    }
    pthread_mutex_lock(&registry_lock);
    ThreadEntry *entry = reserve_entry();
    if (entry) {
        entry->start = start;
        entry->argument = argument;
        entry->joinable = joinable;
    }
    pthread_mutex_unlock(&registry_lock);
    if (!entry) {
        if (translated) pthread_attr_destroy(translated);
        return EAGAIN;
    }
    result = pthread_create(&entry->native, translated, native_start, entry);
    if (translated) pthread_attr_destroy(translated);
    if (result) {
        pthread_mutex_lock(&registry_lock);
        entry->occupied = false;
        pthread_mutex_unlock(&registry_lock);
        return result;
    }
    *token = (uint64_t)(uintptr_t)entry;
    return 0;
}

static uint64_t bridge_self(void)
{
    if (ensure_identity() != 0)
        return 0;
    ThreadEntry *entry = pthread_getspecific(identity_key);
    if (entry)
        return (uint64_t)(uintptr_t)entry;

    pthread_mutex_lock(&registry_lock);
    entry = reserve_entry();
    if (entry) {
        entry->native = pthread_self();
        entry->joinable = false;
    }
    pthread_mutex_unlock(&registry_lock);
    if (!entry)
        return 0;
    if (pthread_setspecific(identity_key, entry) != 0)
        return 0;
    return (uint64_t)(uintptr_t)entry;
}

static int bridge_join(uint64_t token, void **value)
{
    pthread_mutex_lock(&registry_lock);
    ThreadEntry *entry = find_entry(token);
    if (!entry || !entry->joinable || entry->joining || entry->consumed) {
        pthread_mutex_unlock(&registry_lock);
        return EINVAL;
    }
    entry->joining = true;
    pthread_t native = entry->native;
    pthread_mutex_unlock(&registry_lock);

    int result = pthread_join(native, value);
    pthread_mutex_lock(&registry_lock);
    entry->joining = false;
    if (!result) entry->consumed = true;
    pthread_mutex_unlock(&registry_lock);
    return result;
}

static int bridge_detach(uint64_t token)
{
    pthread_mutex_lock(&registry_lock);
    ThreadEntry *entry = find_entry(token);
    if (!entry || !entry->joinable || entry->joining || entry->consumed) {
        pthread_mutex_unlock(&registry_lock);
        return EINVAL;
    }
    // The host detach call is non-blocking. Holding the registry lock prevents
    // a concurrent join from claiming this handle before it is consumed.
    int result = pthread_detach(entry->native);
    if (!result) entry->consumed = true;
    pthread_mutex_unlock(&registry_lock);
    return result;
}

static int bridge_equal(uint64_t first, uint64_t second)
{
    return first == second;
}

bool android_thread_lifecycle_compat_resolve(const char *name, const char *version,
                                             const char *library, uint64_t *address)
{
    if (!name || !version || !library || !address ||
        strcmp(version, "LIBC") != 0 || strcmp(library, "libc.so") != 0)
        return false;
    *address = 0;
#define MAP_ANDROID(name_literal, function) \
    if (strcmp(name, name_literal) == 0) \
        *address = (uint64_t)(uintptr_t)&function; else
    MAP_ANDROID("pthread_create", bridge_create)
    MAP_ANDROID("pthread_self", bridge_self)
    MAP_ANDROID("pthread_join", bridge_join)
    MAP_ANDROID("pthread_detach", bridge_detach)
    MAP_ANDROID("pthread_equal", bridge_equal)
    (void)0;
#undef MAP_ANDROID
    return *address != 0;
}
