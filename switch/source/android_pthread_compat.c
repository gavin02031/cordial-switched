#define _GNU_SOURCE
#include "android_pthread_compat.h"

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// Android LP64 pthread_mutex_t and pthread_cond_t are inline, opaque Bionic
// data (40 and 48 bytes). They are NOT ABI-compatible with libnx/newlib.
// https://android.googlesource.com/platform/bionic/+/main/libc/include/pthread.h
// Map each Android object's address to its own native synchronization object.
// The registry lock protects lookup, creation, destruction and all usage counts.
typedef enum { BRIDGE_MUTEX, BRIDGE_COND } BridgeKind;
typedef struct BridgeEntry {
    void *android_address;
    BridgeKind kind;
    union {
        pthread_mutex_t mutex;
        pthread_cond_t cond;
    } native;
    unsigned in_flight;
    unsigned held;
    struct BridgeEntry *next;
} BridgeEntry;

enum { BRIDGE_MAX_OBJECTS = 16384 };
static pthread_mutex_t registry_lock = PTHREAD_MUTEX_INITIALIZER;
static BridgeEntry *registry;
static unsigned registry_count;

static BridgeEntry *find_locked(void *address, BridgeKind kind)
{
    for (BridgeEntry *entry = registry; entry; entry = entry->next) {
        if (entry->android_address == address && entry->kind == kind)
            return entry;
    }
    return NULL;
}

// Bionic's normal/recursive/errorcheck static mutex initializers start with
// (type & 3) << 14, i.e. 0, 0x4000 and 0x8000. Do not guess about other
// patterns, process-shared objects, or mutexes already used by Bionic itself.
static int mutex_type_for_static(void *address, int *type)
{
    uint32_t marker;
    memcpy(&marker, address, sizeof(marker));
    if (marker == 0) *type = 0;
    else if (marker == 0x4000) *type = 1;
    else if (marker == 0x8000) *type = 2;
    else return ENOTSUP;
    return 0;
}

static int native_mutex_type(int bionic_type)
{
    switch (bionic_type) {
    case 0: return PTHREAD_MUTEX_NORMAL;
    case 1: return PTHREAD_MUTEX_RECURSIVE;
    case 2: return PTHREAD_MUTEX_ERRORCHECK;
    default: return -1;
    }
}

static int create_locked(void *address, BridgeKind kind, int mutex_type,
                         BridgeEntry **result)
{
    if (registry_count == BRIDGE_MAX_OBJECTS)
        return ENOMEM;
    BridgeEntry *entry = calloc(1, sizeof(*entry));
    if (!entry)
        return ENOMEM;
    int error = 0;
    if (kind == BRIDGE_MUTEX) {
        pthread_mutexattr_t attributes;
        error = pthread_mutexattr_init(&attributes);
        if (!error) {
            error = pthread_mutexattr_settype(&attributes, native_mutex_type(mutex_type));
            if (!error)
                error = pthread_mutex_init(&entry->native.mutex, &attributes);
            pthread_mutexattr_destroy(&attributes);
        }
    } else {
        error = pthread_cond_init(&entry->native.cond, NULL);
    }
    if (error) {
        free(entry);
        return error;
    }
    entry->android_address = address;
    entry->kind = kind;
    entry->next = registry;
    registry = entry;
    registry_count++;
    *result = entry;
    return 0;
}

// Must be called with registry_lock held. The caller receives a pointer whose
// lifetime is reserved by incrementing in_flight before releasing that lock.
static int acquire_locked(void *address, BridgeKind kind, BridgeEntry **result)
{
    if (!address)
        return EINVAL;
    BridgeEntry *entry = find_locked(address, kind);
    if (!entry) {
        int type = 0;
        if (kind == BRIDGE_MUTEX) {
            int error = mutex_type_for_static(address, &type);
            if (error)
                return error;
        } else {
            uint32_t marker;
            memcpy(&marker, address, sizeof(marker));
            if (marker != 0) return ENOTSUP;
        }
        int error = create_locked(address, kind, type, &entry);
        if (error)
            return error;
    }
    entry->in_flight++;
    *result = entry;
    return 0;
}

static int android_mutexattr_init(void *attr)
{
    if (!attr) return EINVAL;
    *(long *)attr = 0;
    return 0;
}

static int android_mutexattr_settype(void *attr, int type)
{
    if (!attr || native_mutex_type(type) < 0) return EINVAL;
    long *flags = attr;
    *flags = (*flags & ~15L) | type;
    return 0;
}

static int android_mutexattr_destroy(void *attr)
{
    if (!attr) return EINVAL;
    *(long *)attr = 0;
    return 0;
}

static int android_mutex_init(void *address, const void *attr)
{
    if (!address) return EINVAL;
    long flags = attr ? *(const long *)attr : 0;
    // Don't claim process-shared, robust, or priority-inheritance semantics.
    if ((flags & ~15L) != 0 || native_mutex_type((int)(flags & 15)) < 0)
        return ENOTSUP;
    pthread_mutex_lock(&registry_lock);
    BridgeEntry *entry = NULL;
    int error = find_locked(address, BRIDGE_MUTEX) ? EBUSY :
                create_locked(address, BRIDGE_MUTEX, (int)(flags & 15),
                              &entry);
    pthread_mutex_unlock(&registry_lock);
    return error;
}

static int mutex_take(void *address, bool try_only)
{
    pthread_mutex_lock(&registry_lock);
    BridgeEntry *entry = NULL;
    int error = acquire_locked(address, BRIDGE_MUTEX, &entry);
    pthread_mutex_unlock(&registry_lock);
    if (error) return error;

    error = try_only ? pthread_mutex_trylock(&entry->native.mutex) :
                       pthread_mutex_lock(&entry->native.mutex);
    pthread_mutex_lock(&registry_lock);
    if (error == 0) entry->held++;
    entry->in_flight--;
    pthread_mutex_unlock(&registry_lock);
    return error;
}

static int android_mutex_lock(void *address) { return mutex_take(address, false); }
static int android_mutex_trylock(void *address) { return mutex_take(address, true); }

static int android_mutex_unlock(void *address)
{
    if (!address) return EINVAL;
    pthread_mutex_lock(&registry_lock);
    BridgeEntry *entry = find_locked(address, BRIDGE_MUTEX);
    if (!entry || !entry->held) {
        pthread_mutex_unlock(&registry_lock);
        return EPERM;
    }
    entry->in_flight++;
    pthread_mutex_unlock(&registry_lock);

    int error = pthread_mutex_unlock(&entry->native.mutex);
    pthread_mutex_lock(&registry_lock);
    if (!error) entry->held--;
    entry->in_flight--;
    pthread_mutex_unlock(&registry_lock);
    return error;
}

static void unlink_locked(BridgeEntry *entry)
{
    BridgeEntry **link = &registry;
    while (*link && *link != entry) link = &(*link)->next;
    if (*link) {
        *link = entry->next;
        registry_count--;
        free(entry);
    }
}

static int android_mutex_destroy(void *address)
{
    if (!address) return EINVAL;
    pthread_mutex_lock(&registry_lock);
    BridgeEntry *entry = NULL;
    int error = acquire_locked(address, BRIDGE_MUTEX, &entry);
    if (!error) {
        entry->in_flight--;
        if (entry->in_flight || entry->held)
            error = EBUSY;
        else {
            error = pthread_mutex_destroy(&entry->native.mutex);
            if (!error) unlink_locked(entry);
        }
    }
    pthread_mutex_unlock(&registry_lock);
    return error;
}

static int android_cond_init(void *address, const void *attr)
{
    if (!address) return EINVAL;
    // The default Bionic cond clock is realtime. Additional attributes need
    // an explicit translation before their calls can be exposed.
    if (attr && *(const long *)attr != 0) return ENOTSUP;
    pthread_mutex_lock(&registry_lock);
    BridgeEntry *entry = NULL;
    int error = find_locked(address, BRIDGE_COND) ? EBUSY :
                create_locked(address, BRIDGE_COND, 0, &entry);
    pthread_mutex_unlock(&registry_lock);
    return error;
}

static int cond_notify(void *address, bool broadcast)
{
    pthread_mutex_lock(&registry_lock);
    BridgeEntry *entry = NULL;
    int error = acquire_locked(address, BRIDGE_COND, &entry);
    pthread_mutex_unlock(&registry_lock);
    if (error) return error;
    error = broadcast ? pthread_cond_broadcast(&entry->native.cond) :
                        pthread_cond_signal(&entry->native.cond);
    pthread_mutex_lock(&registry_lock);
    entry->in_flight--;
    pthread_mutex_unlock(&registry_lock);
    return error;
}

static int android_cond_signal(void *address) { return cond_notify(address, false); }
static int android_cond_broadcast(void *address) { return cond_notify(address, true); }

static int android_cond_wait(void *cond_address, void *mutex_address)
{
    if (!cond_address || !mutex_address) return EINVAL;
    pthread_mutex_lock(&registry_lock);
    BridgeEntry *cond = NULL, *mutex = NULL;
    int error = acquire_locked(cond_address, BRIDGE_COND, &cond);
    if (!error) {
        error = acquire_locked(mutex_address, BRIDGE_MUTEX, &mutex);
        if (error) cond->in_flight--;
    }
    if (!error && !mutex->held) {
        mutex->in_flight--;
        cond->in_flight--;
        error = EPERM;
    }
    pthread_mutex_unlock(&registry_lock);
    if (error) return error;

    error = pthread_cond_wait(&cond->native.cond, &mutex->native.mutex);
    pthread_mutex_lock(&registry_lock);
    mutex->in_flight--;
    cond->in_flight--;
    pthread_mutex_unlock(&registry_lock);
    return error;
}

static int android_cond_destroy(void *address)
{
    if (!address) return EINVAL;
    pthread_mutex_lock(&registry_lock);
    BridgeEntry *entry = NULL;
    int error = acquire_locked(address, BRIDGE_COND, &entry);
    if (!error) {
        entry->in_flight--;
        if (entry->in_flight) error = EBUSY;
        else {
            error = pthread_cond_destroy(&entry->native.cond);
            if (!error) unlink_locked(entry);
        }
    }
    pthread_mutex_unlock(&registry_lock);
    return error;
}

static int android_once(volatile int *control, void (*initialise)(void))
{
    if (!control || !initialise) return EINVAL;
    int expected = 0;
    if (__atomic_compare_exchange_n(control, &expected, 1, false,
                                    __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        initialise();
        __atomic_store_n(control, 2, __ATOMIC_RELEASE);
        return 0;
    }
    // Android once values are 0=uninitialized, 1=running, 2=completed.
    while (expected == 1) {
        sched_yield();
        expected = __atomic_load_n(control, __ATOMIC_ACQUIRE);
    }
    return expected == 2 ? 0 : EINVAL;
}

bool android_pthread_compat_resolve(const char *name, const char *version,
                                   const char *library, uint64_t *address)
{
    if (!name || !version || !library || !address ||
        strcmp(version, "LIBC") != 0 || strcmp(library, "libc.so") != 0)
        return false;
    *address = 0;
#define MAP_ANDROID(name_suffix, function) \
    if (strcmp(name, "pthread_" name_suffix) == 0) \
        *address = (uint64_t)(uintptr_t)&function; else
    MAP_ANDROID("mutexattr_init", android_mutexattr_init)
    MAP_ANDROID("mutexattr_settype", android_mutexattr_settype)
    MAP_ANDROID("mutexattr_destroy", android_mutexattr_destroy)
    MAP_ANDROID("mutex_init", android_mutex_init)
    MAP_ANDROID("mutex_lock", android_mutex_lock)
    MAP_ANDROID("mutex_trylock", android_mutex_trylock)
    MAP_ANDROID("mutex_unlock", android_mutex_unlock)
    MAP_ANDROID("mutex_destroy", android_mutex_destroy)
    MAP_ANDROID("cond_init", android_cond_init)
    MAP_ANDROID("cond_wait", android_cond_wait)
    MAP_ANDROID("cond_signal", android_cond_signal)
    MAP_ANDROID("cond_broadcast", android_cond_broadcast)
    MAP_ANDROID("cond_destroy", android_cond_destroy)
    MAP_ANDROID("once", android_once)
    (void)0;
#undef MAP_ANDROID
    return *address != 0;
}
