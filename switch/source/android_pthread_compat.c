#define _GNU_SOURCE
#include "android_pthread_compat.h"

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

_Static_assert(sizeof(struct timespec) == 16, "Android LP64 timespec layout");
_Static_assert(sizeof(time_t) == 8, "Android LP64 time_t layout");

// Android LP64 pthread_mutex_t and pthread_cond_t are inline, opaque Bionic
// data (40 and 48 bytes). They are NOT ABI-compatible with libnx/newlib.
// https://android.googlesource.com/platform/bionic/+/main/libc/include/pthread.h
// Map each Android object's address to its own native synchronization object.
// The registry lock protects lookup, creation, destruction and all usage counts.
typedef enum { BRIDGE_MUTEX, BRIDGE_COND, BRIDGE_RWLOCK } BridgeKind;

typedef struct BridgeReader {
    pthread_t thread;
    unsigned holds;
    struct BridgeReader *next;
} BridgeReader;

typedef struct {
    pthread_mutex_t control;
    pthread_cond_t wake;
    BridgeReader *readers;
    unsigned active_readers;
    unsigned waiting_writers;
    bool writer_active;
    pthread_t writer;
} BridgeRwlock;

typedef struct BridgeEntry {
    void *android_address;
    BridgeKind kind;
    union {
        pthread_mutex_t mutex;
        pthread_cond_t cond;
        BridgeRwlock rwlock;
    } native;
    unsigned in_flight;
    unsigned held;
    struct BridgeEntry *next;
} BridgeEntry;

enum { BRIDGE_MAX_OBJECTS = 16384 };
static pthread_mutex_t registry_lock = PTHREAD_MUTEX_INITIALIZER;
// Engine worker threads revisit the same locks frequently. Hash by guest
// address so each lock/unlock does not scan every registered engine object.
// The registry mutex still protects buckets and lifetime counters together.
enum { BRIDGE_BUCKETS = 1024 };
static BridgeEntry *registry[BRIDGE_BUCKETS];

static unsigned registry_bucket(const void *address, BridgeKind kind)
{
    uintptr_t key = (uintptr_t)address >> 2;
    key ^= key >> 17;
    key *= UINT64_C(0x9e3779b97f4a7c15);
    key ^= key >> 32;
    return (unsigned)(key ^ (unsigned)kind) & (BRIDGE_BUCKETS - 1);
}
static unsigned registry_count;

static BridgeEntry *find_locked(void *address, BridgeKind kind)
{
    for (BridgeEntry *entry = registry[registry_bucket(address, kind)]; entry; entry = entry->next) {
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

static int create_locked(void *address, BridgeKind kind, int configuration,
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
            error = pthread_mutexattr_settype(&attributes, native_mutex_type(configuration));
            if (!error)
                error = pthread_mutex_init(&entry->native.mutex, &attributes);
            pthread_mutexattr_destroy(&attributes);
        }
    } else if (kind == BRIDGE_COND) {
        pthread_condattr_t attributes;
        error = pthread_condattr_init(&attributes);
        if (!error) {
            // Bionic encodes the clock in bit 1, not the host clock ID.
            // devkitPro libsysbase preserves this clock through timedwait.
            error = pthread_condattr_setclock(&attributes,
                        configuration ? CLOCK_MONOTONIC : CLOCK_REALTIME);
            if (!error) error = pthread_cond_init(&entry->native.cond, &attributes);
            pthread_condattr_destroy(&attributes);
        }
    } else {
        error = pthread_mutex_init(&entry->native.rwlock.control, NULL);
        if (!error) {
            error = pthread_cond_init(&entry->native.rwlock.wake, NULL);
            if (error) pthread_mutex_destroy(&entry->native.rwlock.control);
        }
    }
    if (error) {
        free(entry);
        return error;
    }
    entry->android_address = address;
    entry->kind = kind;
    unsigned bucket = registry_bucket(address, kind);
    entry->next = registry[bucket];
    registry[bucket] = entry;
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
            // Bionic also has a monotonic static cond initialiser (2).
            // Never interpret its shared bit or live futex counter as a clock.
            uint32_t marker;
            memcpy(&marker, address, sizeof(marker));
            if (kind == BRIDGE_COND && marker == 2) type = 1;
            else if (marker != 0) return ENOTSUP;
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
    BridgeEntry **link = &registry[registry_bucket(entry->android_address, entry->kind)];
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
    long flags = attr ? *(const long *)attr : 0;
    if (flags & ~2L) return ENOTSUP;
    pthread_mutex_lock(&registry_lock);
    BridgeEntry *entry = NULL;
    int error = find_locked(address, BRIDGE_COND) ? EBUSY :
                create_locked(address, BRIDGE_COND, (int)(flags >> 1), &entry);
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

static int android_cond_wait_internal(void *cond_address, void *mutex_address,
                                      const struct timespec *deadline);

static int android_cond_wait(void *cond_address, void *mutex_address)
{
    return android_cond_wait_internal(cond_address, mutex_address, NULL);
}

static int android_cond_wait_internal(void *cond_address, void *mutex_address,
                                      const struct timespec *deadline)
{
    if (!cond_address || !mutex_address) return EINVAL;
    if (deadline && (deadline->tv_nsec < 0 || deadline->tv_nsec >= 1000000000L))
        return EINVAL;
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

    error = deadline ? pthread_cond_timedwait(&cond->native.cond,
                                             &mutex->native.mutex, deadline) :
                       pthread_cond_wait(&cond->native.cond, &mutex->native.mutex);
    pthread_mutex_lock(&registry_lock);
    mutex->in_flight--;
    cond->in_flight--;
    pthread_mutex_unlock(&registry_lock);
    return error;
}

static int android_cond_timedwait(void *cond_address, void *mutex_address,
                                  const struct timespec *deadline)
{
    if (!deadline) return EINVAL;
    return android_cond_wait_internal(cond_address, mutex_address, deadline);
}

static int android_condattr_init(void *attr)
{
    if (!attr) return EINVAL;
    *(long *)attr = 0;
    return 0;
}

static int android_condattr_setclock(void *attr, int clock)
{
    if (!attr) return EINVAL;
    if (*(long *)attr & ~2L) return ENOTSUP;
    if (clock != 0 && clock != 1) return EINVAL;
    *(long *)attr = (long)clock << 1;
    return 0;
}

static int android_condattr_destroy(void *attr)
{
    if (!attr) return EINVAL;
    *(long *)attr = 0;
    return 0;
}

// Reader and writer ownership are maintained independently of newlib's opaque
// pthread_rwlock_t. This also avoids assuming newlib supplies rwlock calls.
static BridgeReader *rw_reader(BridgeRwlock *lock, pthread_t thread)
{
    for (BridgeReader *reader = lock->readers; reader; reader = reader->next)
        if (pthread_equal(reader->thread, thread)) return reader;
    return NULL;
}

static int rw_acquire(void *address, bool write)
{
    pthread_mutex_lock(&registry_lock);
    BridgeEntry *entry = NULL;
    int error = acquire_locked(address, BRIDGE_RWLOCK, &entry);
    pthread_mutex_unlock(&registry_lock);
    if (error) return error;

    BridgeRwlock *lock = &entry->native.rwlock;
    pthread_t self = pthread_self();
    pthread_mutex_lock(&lock->control);
    if (lock->writer_active && pthread_equal(lock->writer, self)) {
        error = EDEADLK;
    } else if (write && rw_reader(lock, self)) {
        // Upgrading a held read lock would wait forever for our own release.
        error = EDEADLK;
    } else if (write) {
        lock->waiting_writers++;
        while (lock->writer_active || lock->active_readers)
            pthread_cond_wait(&lock->wake, &lock->control);
        lock->waiting_writers--;
        lock->writer_active = true;
        lock->writer = self;
    } else {
        BridgeReader *reader = rw_reader(lock, self);
        // A reader already holding this lock must be able to acquire it
        // recursively even after a writer queues, or it deadlocks itself.
        while (lock->writer_active || (lock->waiting_writers && !reader))
            pthread_cond_wait(&lock->wake, &lock->control);
        reader = rw_reader(lock, self);
        if (!reader) {
            reader = calloc(1, sizeof(*reader));
            if (reader) {
                reader->thread = self;
                reader->next = lock->readers;
                lock->readers = reader;
            }
        }
        if (!reader) error = ENOMEM;
        else {
            reader->holds++;
            lock->active_readers++;
        }
    }
    pthread_mutex_unlock(&lock->control);
    pthread_mutex_lock(&registry_lock);
    entry->in_flight--;
    pthread_mutex_unlock(&registry_lock);
    return error;
}

static int android_rwlock_rdlock(void *address) { return rw_acquire(address, false); }
static int android_rwlock_wrlock(void *address) { return rw_acquire(address, true); }

static int android_rwlock_init(void *address, const void *attr)
{
    if (!address) return EINVAL;
    if (attr && *(const long *)attr != 0) return ENOTSUP;
    pthread_mutex_lock(&registry_lock);
    BridgeEntry *entry = NULL;
    int error = find_locked(address, BRIDGE_RWLOCK) ? EBUSY :
                create_locked(address, BRIDGE_RWLOCK, 0, &entry);
    pthread_mutex_unlock(&registry_lock);
    return error;
}

static int android_rwlock_unlock(void *address)
{
    pthread_mutex_lock(&registry_lock);
    BridgeEntry *entry = NULL;
    int error = acquire_locked(address, BRIDGE_RWLOCK, &entry);
    pthread_mutex_unlock(&registry_lock);
    if (error) return error;
    BridgeRwlock *lock = &entry->native.rwlock;
    pthread_mutex_lock(&lock->control);
    pthread_t self = pthread_self();
    if (lock->writer_active && pthread_equal(lock->writer, self)) {
        lock->writer_active = false;
        pthread_cond_broadcast(&lock->wake);
    } else {
        BridgeReader **link = &lock->readers;
        while (*link && !pthread_equal((*link)->thread, self))
            link = &(*link)->next;
        if (!*link) error = EPERM;
        else {
            BridgeReader *reader = *link;
            reader->holds--;
            lock->active_readers--;
            if (!reader->holds) { *link = reader->next; free(reader); }
            if (!lock->active_readers) pthread_cond_broadcast(&lock->wake);
        }
    }
    pthread_mutex_unlock(&lock->control);
    pthread_mutex_lock(&registry_lock);
    entry->in_flight--;
    pthread_mutex_unlock(&registry_lock);
    return error;
}

static int android_rwlock_destroy(void *address)
{
    pthread_mutex_lock(&registry_lock);
    BridgeEntry *entry = NULL;
    int error = acquire_locked(address, BRIDGE_RWLOCK, &entry);
    if (!error) {
        entry->in_flight--;
        BridgeRwlock *lock = &entry->native.rwlock;
        if (entry->in_flight || lock->active_readers || lock->writer_active ||
            lock->waiting_writers) error = EBUSY;
        else {
            int wake_error = pthread_cond_destroy(&lock->wake);
            int lock_error = wake_error ? 0 : pthread_mutex_destroy(&lock->control);
            error = wake_error ? wake_error : lock_error;
            if (!error) unlink_locked(entry);
        }
    }
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
    MAP_ANDROID("cond_timedwait", android_cond_timedwait)
    MAP_ANDROID("condattr_init", android_condattr_init)
    MAP_ANDROID("condattr_setclock", android_condattr_setclock)
    MAP_ANDROID("condattr_destroy", android_condattr_destroy)
    MAP_ANDROID("cond_signal", android_cond_signal)
    MAP_ANDROID("cond_broadcast", android_cond_broadcast)
    MAP_ANDROID("cond_destroy", android_cond_destroy)
    MAP_ANDROID("rwlock_init", android_rwlock_init)
    MAP_ANDROID("rwlock_rdlock", android_rwlock_rdlock)
    MAP_ANDROID("rwlock_wrlock", android_rwlock_wrlock)
    MAP_ANDROID("rwlock_unlock", android_rwlock_unlock)
    MAP_ANDROID("rwlock_destroy", android_rwlock_destroy)
    MAP_ANDROID("once", android_once)
    (void)0;
#undef MAP_ANDROID
    return *address != 0;
}
