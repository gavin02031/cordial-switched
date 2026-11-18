#define _POSIX_C_SOURCE 200809L
#include "android_timerfd_compat.h"

#include <errno.h>
#include <pthread.h>
#include <string.h>
#include <time.h>

#include "android_eventfd_compat.h"

// Linux timerfd semantics, lazily evaluated: the object records the arm
// time and interval in nanoseconds; reads and readiness queries fold
// elapsed periods into the expiration counter when they run. Nothing
// ticks in the background, so a blocked read or epoll wait simply needs
// a deadline - which the shared change wait supplies
// (android_timerfd_next_expiry_ms caps the wait slice so the waiter
// wakes at expiry without a signal).
//
// Semantic contract, checked against the Linux timerfd man pages and
// kernel behavior (fs/timerfd.c):
// - Intervals are honored at nanosecond resolution. There is no
//   minimum-period floor; a 500us interval expiries twice per
//   millisecond exactly as Linux does.
// - A disarm (value 0) stops future expirations but keeps the
//   undelivered count readable, as Linux does.
// - A one-shot timer disarms after expiring; the count stays readable.
// - CLOCK_REALTIME objects settle against the current realtime clock,
//   so a forward clock jump folds the skipped span (immediate expiry)
//   and a backward jump delays, matching Linux hrtimer behavior for
//   timers without TFD_TIMER_CANCEL_ON_SET. CANCEL_ON_SET itself is
//   not implemented and settime refuses it with EINVAL rather than
//   accepting and ignoring it.
// - The EPOLLET edge source is the count of readiness transitions
//   (unread expirations going from zero to positive), NOT the total
//   number of firings: on Linux, extra expirations while a count is
//   still unread do not create new edge-triggered events.

#define TIMERFD_MAX_OBJECTS 32
#define TIMERFD_FD_BASE 0x3FFE8000 // Clear of eventfd and epoll ranges.
#define TIMERFD_FD_GENERATIONS 1024
#define NS_PER_SEC 1000000000LL
#define NS_PER_MS 1000000LL

typedef struct {
    bool in_use;
    bool nonblock;
    int clockid;
    int current_fd;
    bool armed;
    int64_t next_expiry_ns; // Absolute, on the object's clock.
    int64_t interval_ns;
    uint64_t expirations;   // Undelivered count (reads reset this).
    uint64_t readiness_edges; // Bumps only when expirations goes 0 -> >0.
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

static int64_t clock_now_ns(int clockid)
{
    struct timespec now;
    if (clock_gettime(clockid == 0 ? CLOCK_REALTIME : CLOCK_MONOTONIC,
                      &now) != 0)
        return 0;
    return (int64_t)now.tv_sec * NS_PER_SEC + now.tv_nsec;
}

// sec/nsec pair to nanoseconds, saturating: Linux accepts very large
// second counts; anything past the int64 ns range is effectively
// "never" (about 292 years), so clamp instead of overflowing.
static int64_t spec_to_ns(int64_t sec, int64_t nsec)
{
    if (sec > (INT64_MAX - nsec) / NS_PER_SEC) return INT64_MAX;
    return sec * NS_PER_SEC + nsec;
}

// Caller holds the object lock. Folds elapsed periods into expirations.
static void settle(VirtualTimerFd *object)
{
    if (!object->armed) return;
    const int64_t now = clock_now_ns(object->clockid);
    if (now < object->next_expiry_ns) return;
    uint64_t periods;
    if (object->interval_ns > 0) {
        periods = (uint64_t)((now - object->next_expiry_ns) /
                             object->interval_ns) + 1;
        object->next_expiry_ns += (int64_t)periods * object->interval_ns;
    } else {
        periods = 1;
        object->armed = false; // One-shot: delivered, disarmed.
    }
    if (object->expirations == 0)
        object->readiness_edges++; // The EPOLLET transition.
    object->expirations += periods;
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
    object->interval_ns = 0;
    object->expirations = 0;
    object->readiness_edges = 0;
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
    return sec >= 0 && nsec >= 0 && nsec < NS_PER_SEC;
}

int android_timerfd_settime(int fd, int flags,
                            const AndroidItimerspec *next,
                            AndroidItimerspec *old)
{
    VirtualTimerFd *object = object_for(fd);
    if (!object) { errno = EBADF; return -1; }
    // TFD_TIMER_CANCEL_ON_SET (0x2) is explicitly unsupported: refusing
    // with EINVAL is honest, accepting and ignoring would not be.
    if (!next ||
        (flags & ~(ANDROID_TFD_TIMER_ABSTIME)) ||
        !valid_spec(next->value_sec, next->value_nsec) ||
        !valid_spec(next->interval_sec, next->interval_nsec)) {
        errno = EINVAL;
        return -1;
    }
    pthread_mutex_lock(&object->lock);
    settle(object);
    if (old) {
        const int64_t now = clock_now_ns(object->clockid);
        int64_t remaining = 0;
        if (object->armed && object->next_expiry_ns > now)
            remaining = object->next_expiry_ns - now;
        old->value_sec = remaining / NS_PER_SEC;
        old->value_nsec = remaining % NS_PER_SEC;
        old->interval_sec = object->interval_ns / NS_PER_SEC;
        old->interval_nsec = object->interval_ns % NS_PER_SEC;
    }
    const int64_t value_ns = spec_to_ns(next->value_sec, next->value_nsec);
    if (value_ns == 0) {
        object->armed = false; // Disarm; delivered expirations survive.
        object->interval_ns = 0;
    } else {
        object->interval_ns = spec_to_ns(next->interval_sec,
                                         next->interval_nsec);
        object->next_expiry_ns = (flags & ANDROID_TFD_TIMER_ABSTIME)
            ? value_ns
            : clock_now_ns(object->clockid) + value_ns;
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
        if (object->nonblock) {
            pthread_mutex_unlock(&object->lock);
            errno = EAGAIN;
            return -1;
        }
        if (!object->armed) {
            // Linux: a disarmed timer's read blocks; the only wake
            // source is a re-arm (or close), so wait on the change
            // signal with no deadline.
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
        const int64_t now = clock_now_ns(object->clockid);
        int64_t wait_ns = object->next_expiry_ns - now;
        if (wait_ns < 1) wait_ns = 1;
        struct timespec deadline;
        clock_gettime(CLOCK_REALTIME, &deadline);
        deadline.tv_sec += wait_ns / NS_PER_SEC;
        deadline.tv_nsec += wait_ns % NS_PER_SEC;
        if (deadline.tv_nsec >= NS_PER_SEC) {
            deadline.tv_sec++;
            deadline.tv_nsec -= NS_PER_SEC;
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
    // read_edges counts 0 -> positive transitions of the unread count:
    // extra firings while a count is still unread are not new edges.
    if (read_edges) *read_edges = object->readiness_edges;
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
    if (object->armed) {
        const int64_t now = clock_now_ns(object->clockid);
        int64_t remaining = object->next_expiry_ns - now;
        if (remaining < 0) remaining = 0;
        // Round up so the waiter never wakes early; up to one
        // millisecond late, which matches epoll's own ms granularity.
        result = (remaining + NS_PER_MS - 1) / NS_PER_MS;
    }
    pthread_mutex_unlock(&object->lock);
    return result;
}
