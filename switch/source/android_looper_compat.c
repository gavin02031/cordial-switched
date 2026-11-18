#define _POSIX_C_SOURCE 200809L
#include "android_looper_compat.h"

#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "android_eventfd_compat.h"
#include "android_log_compat.h"

#include <time.h>

#ifdef __SWITCH__
#include <switch.h>
#endif

enum {
    ALOOPER_ALLOW_NON_CALLBACKS = 1,
    ALOOPER_EVENT_INPUT = 1,
    ALOOPER_EVENT_OUTPUT = 2,
    ALOOPER_EVENT_ERROR = 4,
    ALOOPER_EVENT_HANGUP = 8,
    ALOOPER_EVENT_INVALID = 16,
    ALOOPER_POLL_WAKE = -1,
    ALOOPER_POLL_CALLBACK = -2,
    ALOOPER_POLL_TIMEOUT = -3,
    ALOOPER_POLL_ERROR = -4,
    ALOOPER_MAX_FDS = 128,
};

typedef int (*LooperCallback)(int fd, int events, void *data);

typedef struct {
    int fd;
    int ident;
    int events;
    LooperCallback callback;
    void *data;
    uint64_t generation;
} LooperFd;

typedef struct {
    pthread_mutex_t lock;
    unsigned references;
    int options;
    size_t count;
    uint64_t generation;
    LooperFd registrations[ALOOPER_MAX_FDS];
} NativeLooper;

static pthread_key_t looper_thread_key;
static pthread_once_t looper_key_once = PTHREAD_ONCE_INIT;
static bool looper_key_available;

static void looper_release(NativeLooper *looper)
{
    if (!looper) return;
    pthread_mutex_lock(&looper->lock);
    const bool free_now = looper->references && --looper->references == 0;
    pthread_mutex_unlock(&looper->lock);
    if (free_now) {
        pthread_mutex_destroy(&looper->lock);
        free(looper);
    }
}

static void looper_thread_cleanup(void *value)
{
    looper_release(value);
}

static void create_looper_thread_key(void)
{
    looper_key_available = pthread_key_create(&looper_thread_key,
                                              looper_thread_cleanup) == 0;
}

static NativeLooper *looper_for_thread(void)
{
    if (pthread_once(&looper_key_once, create_looper_thread_key) != 0 ||
        !looper_key_available)
        return NULL;
    return pthread_getspecific(looper_thread_key);
}

static NativeLooper *looper_prepare(int options)
{
    if (pthread_once(&looper_key_once, create_looper_thread_key) != 0 ||
        !looper_key_available)
        return NULL;
    NativeLooper *existing = pthread_getspecific(looper_thread_key);
    if (existing) return existing;
    NativeLooper *looper = calloc(1, sizeof(*looper));
    if (!looper) return NULL;
    if (pthread_mutex_init(&looper->lock, NULL) != 0) {
        free(looper);
        return NULL;
    }
    looper->references = 1; // Thread ownership until its pthread key exits.
    looper->options = options;
    if (pthread_setspecific(looper_thread_key, looper) != 0) {
        looper_release(looper);
        return NULL;
    }
    return looper;
}

static void looper_acquire(NativeLooper *looper)
{
    if (!looper) return;
    pthread_mutex_lock(&looper->lock);
    if (looper->references < UINT_MAX) looper->references++;
    pthread_mutex_unlock(&looper->lock);
}

static int looper_add_fd(NativeLooper *looper, int fd, int ident, int events,
                         LooperCallback callback, void *data)
{
    if (!looper || fd < 0 || !(events & (ALOOPER_EVENT_INPUT | ALOOPER_EVENT_OUTPUT)) ||
        (events & ~(ALOOPER_EVENT_INPUT | ALOOPER_EVENT_OUTPUT)))
        return -1;
    if (!callback && (!(looper->options & ALOOPER_ALLOW_NON_CALLBACKS) || ident < 0))
        return -1;
    pthread_mutex_lock(&looper->lock);
    size_t i = 0;
    for (; i < looper->count; i++) {
        if (looper->registrations[i].fd == fd) break;
    }
    if (i == looper->count && looper->count == ALOOPER_MAX_FDS) {
        pthread_mutex_unlock(&looper->lock);
        return -1;
    }
    const LooperFd registration = {fd, ident, events, callback, data,
                                   ++looper->generation};
    if (i == looper->count) looper->count++;
    looper->registrations[i] = registration;
    pthread_mutex_unlock(&looper->lock);
    return 1;
}

static int looper_remove_fd_generation(NativeLooper *looper, int fd,
                                       uint64_t generation)
{
    if (!looper) return 0;
    pthread_mutex_lock(&looper->lock);
    for (size_t i = 0; i < looper->count; i++) {
        LooperFd *registration = &looper->registrations[i];
        if (registration->fd == fd &&
            (!generation || registration->generation == generation)) {
            looper->count--;
            if (i < looper->count)
                memmove(registration, registration + 1,
                        (looper->count - i) * sizeof(*registration));
            pthread_mutex_unlock(&looper->lock);
            return 1;
        }
    }
    pthread_mutex_unlock(&looper->lock);
    return 0;
}

static int looper_remove_fd(NativeLooper *looper, int fd)
{
    return looper_remove_fd_generation(looper, fd, 0);
}

static short poll_request(int events)
{
    return (short)(((events & ALOOPER_EVENT_INPUT) ? POLLIN : 0) |
                   ((events & ALOOPER_EVENT_OUTPUT) ? POLLOUT : 0));
}

static int android_events(short revents)
{
    int events = 0;
    if (revents & POLLIN) events |= ALOOPER_EVENT_INPUT;
    if (revents & POLLOUT) events |= ALOOPER_EVENT_OUTPUT;
    if (revents & POLLERR) events |= ALOOPER_EVENT_ERROR;
    if (revents & POLLHUP) events |= ALOOPER_EVENT_HANGUP;
    if (revents & POLLNVAL) events |= ALOOPER_EVENT_INVALID;
    return events;
}

// The one place the looper blocks. The guard below exists because two
// silent wedges are possible here and neither may ship:
//
// 1. libnx poll() (nx/source/runtime/devices/socket.c:473) multiplexes
//    ONLY registered BSD sockets: _socketGetFd fails for any other fd
//    and the call returns -1, which would read here as an eternal
//    ALOOPER_POLL_ERROR storm. Morrow currently produces no socket-backed
//    guest fds (eventfd/timerfd/epoll are fail-closed refusals), so a
//    Switch-side wait over registered fds is journaled and refused until
//    a real Horizon wait primitive backs it (event-waiting-design.md).
//
// 2. ALooper_pollOnce(-1) with zero registered fds blocks until
//    ALooper_wake, but no ALooper_wake import is bound, so wake can never
//    arrive: an infinite timeout with nothing to wait on is a guaranteed
//    permanent wedge on ANY platform. Refuse it, logged, on host too.
static int morrow_looper_wait(struct pollfd *polls, size_t count,
                              int timeout_millis)
{
    if (count == 0 && timeout_millis < 0) {
        cordial_android_log_write(6, "looper",
            "looper_wait refused reason=infinite_timeout_no_fds_no_wake");
        errno = EINVAL;
        return -1;
    }
    bool any_virtual = false, any_host = false;
    for (size_t i = 0; i < count; i++) {
        if (polls[i].fd < 0) continue; // poll ignores negative fds
        if (android_eventfd_is_virtual(polls[i].fd)) any_virtual = true;
        else any_host = true;
    }
    if (any_virtual && any_host) {
        // Multiplexing virtual and host descriptors in one wait is not
        // implemented; refuse loudly rather than half-wait.
        static bool logged_mix;
        if (!logged_mix) {
            logged_mix = true;
            cordial_android_log_write(6, "looper",
                "looper_wait refused reason=mixed_virtual_and_host_fds");
        }
        errno = ENOSYS;
        return -1;
    }
    if (any_virtual) {
        // Morrow-virtual descriptors: readiness comes from the objects,
        // identical on host and Horizon; blocking uses the shared change
        // generation. EPOLLIN/EPOLLOUT values match POLLIN/POLLOUT, but
        // map explicitly so the coincidence is not load-bearing.
        struct timespec start;
        clock_gettime(CLOCK_MONOTONIC, &start);
        uint64_t since = android_virtual_generation();
        for (;;) {
            int ready = 0;
            for (size_t i = 0; i < count; i++) {
                polls[i].revents = 0;
                if (polls[i].fd < 0) continue;
                uint32_t live = 0;
                if (!android_eventfd_poll_readiness(polls[i].fd, &live))
                    continue;
                short revents = 0;
                if (live & 0x001) revents |= POLLIN;
                if (live & 0x004) revents |= POLLOUT;
                polls[i].revents = revents & (polls[i].events |
                                              POLLERR | POLLHUP);
                if (polls[i].revents) ready++;
            }
            if (ready > 0 || timeout_millis == 0) return ready;
            int remaining = timeout_millis;
            if (timeout_millis > 0) {
                struct timespec now;
                clock_gettime(CLOCK_MONOTONIC, &now);
                const long elapsed_ms =
                    (now.tv_sec - start.tv_sec) * 1000L +
                    (now.tv_nsec - start.tv_nsec) / 1000000L;
                remaining = timeout_millis - (int)elapsed_ms;
                if (remaining <= 0) return 0;
            }
            since = android_virtual_wait_change(since, remaining);
        }
    }
#ifdef __SWITCH__
    if (count == 0) {
        if (timeout_millis > 0)
            svcSleepThread((uint64_t)timeout_millis * 1000000ULL);
        return 0;
    }
    static bool logged_once;
    if (!logged_once) {
        logged_once = true;
        cordial_android_log_write(6, "looper",
            "looper_wait refused reason=no_horizon_multiplex fds_present=1");
    }
    errno = ENOSYS;
    return -1;
#else
    return poll(polls, count, timeout_millis);
#endif
}

static int looper_poll_once(int timeout_millis, int *out_fd,
                            int *out_events, void **out_data)
{
    if (out_fd) *out_fd = 0;
    if (out_events) *out_events = 0;
    if (out_data) *out_data = NULL;
    NativeLooper *looper = looper_for_thread();
    if (!looper) return ALOOPER_POLL_ERROR;

    LooperFd snapshots[ALOOPER_MAX_FDS];
    struct pollfd polls[ALOOPER_MAX_FDS];
    pthread_mutex_lock(&looper->lock);
    const size_t count = looper->count;
    memcpy(snapshots, looper->registrations, count * sizeof(*snapshots));
    pthread_mutex_unlock(&looper->lock);
    for (size_t i = 0; i < count; i++) {
        polls[i] = (struct pollfd){snapshots[i].fd,
                                    poll_request(snapshots[i].events), 0};
    }
    const int ready = morrow_looper_wait(polls, count, timeout_millis);
    if (ready == 0) return ALOOPER_POLL_TIMEOUT;
    if (ready < 0) return errno == EINTR ? ALOOPER_POLL_WAKE : ALOOPER_POLL_ERROR;

    // One poll() batch may contain several ready callbacks. Run all of them
    // without the registry mutex so callbacks may add/remove registrations.
    bool called = false;
    int first_ident = ALOOPER_POLL_TIMEOUT;
    int first_fd = 0, first_events = 0;
    void *first_data = NULL;
    for (size_t i = 0; i < count; i++) {
        const int events = android_events(polls[i].revents);
        if (!events) continue;
        const LooperFd *item = &snapshots[i];
        pthread_mutex_lock(&looper->lock);
        bool current = false;
        for (size_t j = 0; j < looper->count; j++) {
            if (looper->registrations[j].fd == item->fd &&
                looper->registrations[j].generation == item->generation) {
                current = true;
                break;
            }
        }
        pthread_mutex_unlock(&looper->lock);
        if (!current) continue;

        if (item->callback) {
            called = true;
            if (item->callback(item->fd, events, item->data) == 0)
                looper_remove_fd_generation(looper, item->fd, item->generation);
        } else if (first_ident == ALOOPER_POLL_TIMEOUT) {
            first_ident = item->ident;
            first_fd = item->fd;
            first_events = events;
            first_data = item->data;
        }
    }
    if (called) return ALOOPER_POLL_CALLBACK;
    if (first_ident != ALOOPER_POLL_TIMEOUT) {
        if (out_fd) *out_fd = first_fd;
        if (out_events) *out_events = first_events;
        if (out_data) *out_data = first_data;
        return first_ident;
    }
    return ALOOPER_POLL_WAKE;
}

bool android_looper_compat_resolve(const char *name, const char *version,
                                   const char *library, uint64_t *address)
{
    if (!name || !address || version || library) return false;
    *address = 0;
#define LOOPER_MAP(symbol, implementation) \
    if (strcmp(name, #symbol) == 0) \
        *address = (uint64_t)(uintptr_t)&implementation; else
    LOOPER_MAP(ALooper_prepare, looper_prepare)
    LOOPER_MAP(ALooper_forThread, looper_for_thread)
    LOOPER_MAP(ALooper_acquire, looper_acquire)
    LOOPER_MAP(ALooper_release, looper_release)
    LOOPER_MAP(ALooper_addFd, looper_add_fd)
    LOOPER_MAP(ALooper_removeFd, looper_remove_fd)
    LOOPER_MAP(ALooper_pollOnce, looper_poll_once)
    (void)0;
#undef LOOPER_MAP
    return *address != 0;
}
