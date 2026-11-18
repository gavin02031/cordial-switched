#define _POSIX_C_SOURCE 200809L
#include "android_timerfd_compat.h"

#include <errno.h>
#include <pthread.h>
#include <string.h>
#include <time.h>

#include "android_eventfd_compat.h"

// Linux timerfd semantics, lazily evaluated: the object records the arm
// time and interval; reads and readiness queries fold elapsed periods
// into the expiration counter when they run. Nothing ticks in the
// background, so a blocked read or epoll wait simply needs a deadline -
// which the shared change wait supplies (android_timerfd_next_expiry_ms
// caps the wait slice so the waiter wakes at expiry without a signal).

#define TIMERFD_MAX_OBJECTS 32
#define TIMERFD_FD_BASE 0x3FFE8000 // Clear of eventfd and epoll ranges.
#define TIMERFD_FD_GENERATIONS 1024

typedef struct {
    bool in_use;
    bool nonblock;
    int clockid;
    int current_fd;
    bool armed;
    int64_t next_expiry_ms;  // Absolute, on the object's clock.
    int64_t interval_ms;
    uint64_t expirations;    // Undelivered count (reads reset this).
    uint64_t total_expirations; // Monotone while armed; the ET edge source.
    pthread_mutex_t lock;
    pthread_cond_t changed;
} VirtualTimerFd;

static VirtualTimerFd objects[TIMERFD_MAX_OBJECTS];
static pthread_mutex_t table_lock = PTHREAD_MUTEX_INITIALIZER;
static bool table_initialized;

static void table_init(void)
{
    pthread_mutex_lock(&table_lock);
    if (!table_initialized) {
        for (int i = 0; i < TIMERFD_MAX_OBJECTS; i++) {
            pthread_mutex_init(&objects[i].lock, NULL);
            pthread_cond_init(&objects[i].changed, NULL);
        }
        table_initialized = true;
    }
    pthread_mutex_unlock(&table_lock);
}

static VirtualTimerFd *object_for(int fd)
{
    const int relative = fd - TIMERFD_FD_BASE;
    if (relative < 0 ||
        relative >= TIMERFD_MAX_OBJECTS * TIMERFD_FD_GENERATIONS)
        return NULL;
    VirtualTimerFd *object = &objects[relative % TIMERFD_MAX_OBJECTS];
    return (object->in_use && object->current_fd == fd) ? object : NULL;
}

static int64_t clock_now_ms(int clockid)
{
    struct timespec now;
    if (clock_gettime(clockid == 0 ? CLOCK_REALTIME : CLOCK_MONOTONIC,
                      &now) != 0)
        return 0;
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

// Caller holds the object lock. Folds elapsed periods into expirations.
static void settle(VirtualTimerFd *object)
{
    if (!object->armed) return;
    const int64_t now = clock_now_ms(object->clockid);
    if (now < object->next_expiry_ms) return;
    if (object->interval_ms > 0) {
        const uint64_t periods =
            (uint64_t)((now - object->next_expiry_ms) /
                       object->interval_ms) + 1;
        object->expirations += periods;
        object->total_expirations += periods;
        object->next_expiry_ms += (int64_t)periods * object->interval_ms;
    } else {
        object->expirations++;
        object->total_expirations++;
        object->armed = false; // One-shot: delivered, disarmed.
    }
}

int android_timerfd_create(int clockid, int flags)
{
    // Only CLOCK_REALTIME (0) and CLOCK_MONOTONIC (1) are honest here;
    // Linux rejects other clocks with EINVAL, and so do we.
    if (clockid != 0 && clockid != 1) { errno = EINVAL; return -1; }
    if (flags & ~(ANDROID_TFD_NONBLOCK | ANDROID_TFD_CLOEXEC)) {
        errno = EINVAL;
        return -1;
    }
    table_init();
    pthread_mutex_lock(&table_lock);
    int slot = -1;
    for (int i = 0; i < TIMERFD_MAX_OBJECTS; i++)
        if (!objects[i].in_use) { slot = i; break; }
    if (slot < 0) {
        pthread_mutex_unlock(&table_lock);
        errno = EMFILE;
        return -1;
    }
    VirtualTimerFd *object = &objects[slot];
    pthread_mutex_lock(&object->lock);
    object->in_use = true;
    object->nonblock = (flags & ANDROID_TFD_NONBLOCK) != 0;
    object->clockid = clockid;
    object->armed = false;
    object->interval_ms = 0;
    object->expirations = 0;
    object->total_expirations = 0;
    int generation = 0;
    if (object->current_fd)
        generation = (object->current_fd - TIMERFD_FD_BASE) /
                     TIMERFD_MAX_OBJECTS + 1;
    if (generation >= TIMERFD_FD_GENERATIONS) {
        pthread_mutex_unlock(&object->lock);
        pthread_mutex_unlock(&table_lock);
        errno = EMFILE;
        return -1;
    }
    object->current_fd = TIMERFD_FD_BASE +
                         generation * TIMERFD_MAX_OBJECTS + slot;
    const int fd = object->current_fd;
    pthread_mutex_unlock(&object->lock);
    pthread_mutex_unlock(&table_lock);
    android_virtual_note_change();
    // TFD_CLOEXEC accepted as a no-op: no exec boundary exists on Horizon.
    return fd;
}

bool android_timerfd_is_virtual(int fd)
{
    const int relative = fd - TIMERFD_FD_BASE;
    return relative >= 0 &&
           relative < TIMERFD_MAX_OBJECTS * TIMERFD_FD_GENERATIONS;
}

static bool valid_spec(int64_t sec, int64_t nsec)
{
    return sec >= 0 && nsec >= 0 && nsec < 1000000000LL;
}

int android_timerfd_settime(int fd, int flags,
                            const AndroidItimerspec *next,
                            AndroidItimerspec *old)
{
    VirtualTimerFd *object = object_for(fd);
    if (!object) { errno = EBADF; return -1; }
    if (!next || (flags & ~ANDROID_TFD_TIMER_ABSTIME) ||
        !valid_spec(next->value_sec, next->value_nsec) ||
        !valid_spec(next->interval_sec, next->interval_nsec)) {
        errno = EINVAL;
        return -1;
    }
    pthread_mutex_lock(&object->lock);
    settle(object);
    if (old) {
        const int64_t now = clock_now_ms(object->clockid);
        int64_t remaining = 0;
        if (object->armed && object->next_expiry_ms > now)
            remaining = object->next_expiry_ms - now;
        old->value_sec = remaining / 1000;
        old->value_nsec = (remaining % 1000) * 1000000LL;
        old->interval_sec = object->interval_ms / 1000;
        old->interval_nsec = (object->interval_ms % 1000) * 1000000LL;
    }
    const int64_t value_ms = next->value_sec * 1000 +
                             next->value_nsec / 1000000;
    if (value_ms == 0) {
        object->armed = false; // Disarm; delivered expirations survive.
        object->interval_ms = 0;
    } else {
        object->interval_ms = next->interval_sec * 1000 +
                              next->interval_nsec / 1000000;
        if (object->interval_ms == 0 &&
            (next->interval_sec || next->interval_nsec))
            object->interval_ms = 1; // Sub-millisecond period floors at 1.
        object->next_expiry_ms = (flags & ANDROID_TFD_TIMER_ABSTIME)
            ? value_ms
            : clock_now_ms(object->clockid) + value_ms;
        object->armed = true;
    }
    pthread_cond_broadcast(&object->changed);
    pthread_mutex_unlock(&object->lock);
    android_virtual_note_change();
    return 0;
}

ssize_t android_timerfd_read(int fd, void *buffer, size_t count)
{
    VirtualTimerFd *object = object_for(fd);
    if (!object) { errno = EBADF; return -1; }
    if (count < 8) { errno = EINVAL; return -1; }
    pthread_mutex_lock(&object->lock);
    for (;;) {
        settle(object);
        if (object->expirations > 0) break;
        if (object->nonblock || !object->armed) {
            // Linux: a disarmed timer's read blocks; with nonblock it is
            // EAGAIN. A blocking read on a disarmed timer has no wake
            // source other than a re-arm, so wait on the change signal.
            if (object->nonblock) {
                pthread_mutex_unlock(&object->lock);
                errno = EAGAIN;
                return -1;
            }
        }
        if (!object->armed) {
            pthread_cond_wait(&object->changed, &object->lock);
            if (!object->in_use) {
                pthread_mutex_unlock(&object->lock);
                errno = EBADF;
                return -1;
            }
            continue;
        }
        // Blocking until expiry: sleep out the remaining span; a re-arm
        // or close signals changed and re-evaluates.
        const int64_t now = clock_now_ms(object->clockid);
        int64_t wait_ms = object->next_expiry_ms - now;
        if (wait_ms < 1) wait_ms = 1;
        struct timespec deadline;
        clock_gettime(CLOCK_REALTIME, &deadline);
        deadline.tv_sec += wait_ms / 1000;
        deadline.tv_nsec += (wait_ms % 1000) * 1000000L;
        if (deadline.tv_nsec >= 1000000000L) {
            deadline.tv_sec++;
            deadline.tv_nsec -= 1000000000L;
        }
        pthread_cond_timedwait(&object->changed, &object->lock, &deadline);
        if (!object->in_use) {
            pthread_mutex_unlock(&object->lock);
            errno = EBADF;
            return -1;
        }
    }
    const uint64_t delivered = object->expirations;
    object->expirations = 0;
    pthread_mutex_unlock(&object->lock);
    memcpy(buffer, &delivered, 8);
    android_virtual_note_change();
    return 8;
}

int android_timerfd_close(int fd)
{
    VirtualTimerFd *object = object_for(fd);
    if (!object) { errno = EBADF; return -1; }
    pthread_mutex_lock(&object->lock);
    object->in_use = false;
    object->armed = false;
    object->expirations = 0;
    pthread_cond_broadcast(&object->changed);
    pthread_mutex_unlock(&object->lock);
    android_virtual_note_change();
    return 0;
}

bool android_timerfd_readiness_edges(int fd, uint32_t *ready_events,
                                     uint64_t *read_edges,
                                     uint64_t *write_edges)
{
    VirtualTimerFd *object = object_for(fd);
    if (!object) return false;
    pthread_mutex_lock(&object->lock);
    settle(object);
    uint32_t ready = 0;
    if (object->expirations > 0) ready |= 0x001; // EPOLLIN
    // read_edges is the monotone expiration total; reads reset only the
    // undelivered count.
    if (read_edges) *read_edges = object->total_expirations;
    if (write_edges) *write_edges = 0;
    if (ready_events) *ready_events = ready;
    pthread_mutex_unlock(&object->lock);
    return true;
}

int64_t android_timerfd_next_expiry_ms(int fd)
{
    VirtualTimerFd *object = object_for(fd);
    if (!object) return -1;
    pthread_mutex_lock(&object->lock);
    int64_t result = -1;
    if (object->armed && object->expirations == 0) {
        const int64_t now = clock_now_ms(object->clockid);
        result = object->next_expiry_ms - now;
        if (result < 0) result = 0;
    }
    pthread_mutex_unlock(&object->lock);
    return result;
}
