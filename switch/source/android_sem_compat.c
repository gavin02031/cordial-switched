#include "android_sem_compat.h"

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// Bionic's sem_t contains private futex state and cannot be passed to
// Horizon/newlib. Keep an independent counting semaphore keyed by the guest
// object's address. Only process-private semaphores are supported: Horizon
// has no Android process-shared futex namespace or signal dispatch.
typedef struct SemEntry {
    void *guest;
    pthread_mutex_t lock;
    pthread_cond_t wake;
    unsigned value;
    unsigned waiters;
    unsigned in_flight;
    struct SemEntry *next;
} SemEntry;

enum { MAX_BRIDGED_SEMAPHORES = 4096 };
static pthread_mutex_t sem_registry_lock = PTHREAD_MUTEX_INITIALIZER;
static SemEntry *sem_registry;
static unsigned sem_count;

static int fail(int code)
{
    errno = code;
    return -1;
}

static SemEntry *lookup_locked(void *address)
{
    for (SemEntry *item = sem_registry; item; item = item->next)
        if (item->guest == address) return item;
    return NULL;
}

static SemEntry *acquire(void *address)
{
    pthread_mutex_lock(&sem_registry_lock);
    SemEntry *entry = lookup_locked(address);
    if (entry) entry->in_flight++;
    pthread_mutex_unlock(&sem_registry_lock);
    return entry;
}

static void release(SemEntry *entry)
{
    pthread_mutex_lock(&sem_registry_lock);
    entry->in_flight--;
    pthread_mutex_unlock(&sem_registry_lock);
}

static int bridge_sem_init(void *address, int process_shared, unsigned initial)
{
    if (!address) return fail(EINVAL);
    if (process_shared) return fail(ENOSYS);
    if (initial > INT_MAX) return fail(EINVAL);
    pthread_mutex_lock(&sem_registry_lock);
    if (lookup_locked(address)) {
        pthread_mutex_unlock(&sem_registry_lock);
        return fail(EBUSY);
    }
    if (sem_count == MAX_BRIDGED_SEMAPHORES) {
        pthread_mutex_unlock(&sem_registry_lock);
        return fail(ENOMEM);
    }
    SemEntry *entry = calloc(1, sizeof(*entry));
    if (!entry) {
        pthread_mutex_unlock(&sem_registry_lock);
        return fail(ENOMEM);
    }
    int error = pthread_mutex_init(&entry->lock, NULL);
    if (!error) {
        error = pthread_cond_init(&entry->wake, NULL);
        if (error) pthread_mutex_destroy(&entry->lock);
    }
    if (error) {
        free(entry);
        pthread_mutex_unlock(&sem_registry_lock);
        return fail(error);
    }
    entry->guest = address;
    entry->value = initial;
    entry->next = sem_registry;
    sem_registry = entry;
    sem_count++;
    pthread_mutex_unlock(&sem_registry_lock);
    return 0;
}

static int bridge_sem_wait(void *address)
{
    SemEntry *entry = acquire(address);
    if (!entry) return fail(EINVAL);
    pthread_mutex_lock(&entry->lock);
    int error = 0;
    while (!entry->value) {
        entry->waiters++;
        error = pthread_cond_wait(&entry->wake, &entry->lock);
        entry->waiters--;
        if (error) break;
    }
    if (!error) entry->value--;
    pthread_mutex_unlock(&entry->lock);
    release(entry);
    return error ? fail(error) : 0;
}

static int bridge_sem_post(void *address)
{
    SemEntry *entry = acquire(address);
    if (!entry) return fail(EINVAL);
    pthread_mutex_lock(&entry->lock);
    int error = 0;
    if (entry->value == INT_MAX) error = EOVERFLOW;
    else {
        entry->value++;
        if (entry->waiters) pthread_cond_signal(&entry->wake);
    }
    pthread_mutex_unlock(&entry->lock);
    release(entry);
    return error ? fail(error) : 0;
}

static int bridge_sem_destroy(void *address)
{
    if (!address) return fail(EINVAL);
    pthread_mutex_lock(&sem_registry_lock);
    SemEntry **link = &sem_registry;
    while (*link && (*link)->guest != address) link = &(*link)->next;
    if (!*link) {
        pthread_mutex_unlock(&sem_registry_lock);
        return fail(EINVAL);
    }
    SemEntry *entry = *link;
    // In-flight calls include blocked waiters. Never free an object before
    // all native threads have finished using its mutex and condition.
    if (entry->in_flight) {
        pthread_mutex_unlock(&sem_registry_lock);
        return fail(EBUSY);
    }
    int error = pthread_cond_destroy(&entry->wake);
    if (!error) error = pthread_mutex_destroy(&entry->lock);
    if (!error) {
        *link = entry->next;
        sem_count--;
        free(entry);
    }
    pthread_mutex_unlock(&sem_registry_lock);
    return error ? fail(error) : 0;
}

bool android_sem_compat_resolve(const char *name, const char *version,
                                const char *library, uint64_t *address)
{
    if (!name || !version || !library || !address ||
        strcmp(version, "LIBC") || strcmp(library, "libc.so")) return false;
    if (!strcmp(name, "sem_init"))
        *address = (uint64_t)(uintptr_t)&bridge_sem_init;
    else if (!strcmp(name, "sem_wait"))
        *address = (uint64_t)(uintptr_t)&bridge_sem_wait;
    else if (!strcmp(name, "sem_post"))
        *address = (uint64_t)(uintptr_t)&bridge_sem_post;
    else if (!strcmp(name, "sem_destroy"))
        *address = (uint64_t)(uintptr_t)&bridge_sem_destroy;
    else
        return false;
    return true;
}
