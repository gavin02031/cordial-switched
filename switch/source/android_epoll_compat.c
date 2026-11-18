#define _POSIX_C_SOURCE 200809L
#include "android_epoll_compat.h"

#include <errno.h>
#include <pthread.h>

#include "android_eventfd_compat.h"

// Linux epoll semantics, subset. Bionic passes the ABI through unchanged
// (aarch64 struct epoll_event is 16 bytes: u32 events + 8-byte data at
// offset 8; flags are the asm-generic values verified against bionic's
// kernel uapi eventpoll.h). The supported set is deliberately narrow and
// every deviation is explicit:
//   - targets must be Morrow-virtual descriptors (eventfd today); a host
//     fd or anything else fails EPERM, as Linux does for fds without a
//     poll method - never silently level-poll a socket or file;
//   - accepted event mask: EPOLLIN, EPOLLOUT, EPOLLERR, EPOLLHUP,
//     EPOLLONESHOT; EPOLLET is accepted but reported level-triggered
//     (documented deviation: edge latching is not implemented);
//   - readiness is recomputed from the live objects on every wake, so a
//     descriptor closed or drained by another thread is never reported
//     from stale state.

#define EPOLLIN_V     0x001u
#define EPOLLOUT_V    0x004u
#define EPOLLERR_V    0x008u
#define EPOLLHUP_V    0x010u
#define EPOLLONESHOT_V (1u << 30)
#define EPOLLET_V     (1u << 31)
#define EPOLL_SUPPORTED (EPOLLIN_V | EPOLLOUT_V | EPOLLERR_V | \
                         EPOLLHUP_V | EPOLLONESHOT_V | EPOLLET_V)

#define EPOLL_MAX_INSTANCES 32
#define EPOLL_MAX_WATCHES 128
#define EPOLL_FD_BASE 0x3FFD1000

typedef struct {
    int fd;
    uint32_t events;
    uint64_t data;
    bool disarmed; // EPOLLONESHOT consumed; rearms on MOD
} EpollWatch;

typedef struct {
    bool in_use;
    EpollWatch watches[EPOLL_MAX_WATCHES];
    int watch_count;
} VirtualEpoll;

static VirtualEpoll instances[EPOLL_MAX_INSTANCES];
static pthread_mutex_t instances_lock = PTHREAD_MUTEX_INITIALIZER;

static VirtualEpoll *instance_for(int epfd)
{
    const int slot = epfd - EPOLL_FD_BASE;
    if (slot < 0 || slot >= EPOLL_MAX_INSTANCES) return NULL;
    VirtualEpoll *instance = &instances[slot];
    return instance->in_use ? instance : NULL;
}

int android_epoll_create(void)
{
    pthread_mutex_lock(&instances_lock);
    int slot = -1;
    for (int i = 0; i < EPOLL_MAX_INSTANCES; i++) {
        if (!instances[i].in_use) { slot = i; break; }
    }
    if (slot < 0) {
        pthread_mutex_unlock(&instances_lock);
        errno = EMFILE;
        return -1;
    }
    instances[slot].in_use = true;
    instances[slot].watch_count = 0;
    pthread_mutex_unlock(&instances_lock);
    return EPOLL_FD_BASE + slot;
}

bool android_epoll_is_virtual(int fd)
{
    return fd >= EPOLL_FD_BASE && fd < EPOLL_FD_BASE + EPOLL_MAX_INSTANCES;
}

// operation: 1 = ADD, 2 = DEL, 3 = MOD (Linux EPOLL_CTL_*).
int android_epoll_ctl(int epfd, int operation, int fd,
                      uint32_t events, uint64_t data)
{
    if (operation == 1 && (events & ~EPOLL_SUPPORTED)) {
        errno = EINVAL;
        return -1;
    }
    pthread_mutex_lock(&instances_lock);
    VirtualEpoll *instance = instance_for(epfd);
    if (!instance) {
        pthread_mutex_unlock(&instances_lock);
        errno = EBADF;
        return -1;
    }
    if (operation == 2) { // DEL: the event argument is ignored (Linux >= 2.6.9)
        for (int i = 0; i < instance->watch_count; i++) {
            if (instance->watches[i].fd == fd) {
                instance->watch_count--;
                if (i < instance->watch_count)
                    instance->watches[i] = instance->watches[instance->watch_count];
                pthread_mutex_unlock(&instances_lock);
                return 0;
            }
        }
        pthread_mutex_unlock(&instances_lock);
        errno = ENOENT;
        return -1;
    }
    // ADD and MOD both require a watchable target. Only Morrow-virtual
    // descriptors qualify; refusing others keeps silent wrong readiness
    // off the console.
    if (!android_eventfd_is_virtual(fd) || !android_eventfd_poll_readiness(fd, NULL)) {
        pthread_mutex_unlock(&instances_lock);
        errno = EPERM;
        return -1;
    }
    int found = -1;
    for (int i = 0; i < instance->watch_count; i++)
        if (instance->watches[i].fd == fd) { found = i; break; }
    if (operation == 1) { // ADD
        if (found >= 0) {
            pthread_mutex_unlock(&instances_lock);
            errno = EEXIST;
            return -1;
        }
        if (instance->watch_count == EPOLL_MAX_WATCHES) {
            pthread_mutex_unlock(&instances_lock);
            errno = ENOSPC;
            return -1;
        }
        EpollWatch *watch = &instance->watches[instance->watch_count++];
        watch->fd = fd;
        watch->events = events;
        watch->data = data;
        watch->disarmed = false;
    } else if (operation == 3) { // MOD
        if (found < 0) {
            pthread_mutex_unlock(&instances_lock);
            errno = ENOENT;
            return -1;
        }
        instance->watches[found].events = events;
        instance->watches[found].data = data;
        instance->watches[found].disarmed = false;
    } else {
        pthread_mutex_unlock(&instances_lock);
        errno = EINVAL;
        return -1;
    }
    pthread_mutex_unlock(&instances_lock);
    // Wake waiters so a freshly armed watch is noticed immediately.
    android_virtual_note_change();
    return 0;
}

int android_epoll_wait(int epfd, uint32_t *out_events, uint64_t *out_data,
                       int maxevents, int timeout_millis)
{
    if (!out_events || !out_data || maxevents <= 0) {
        errno = EINVAL;
        return -1;
    }
    uint64_t since = android_virtual_generation();
    for (;;) {
        int ready = 0;
        pthread_mutex_lock(&instances_lock);
        VirtualEpoll *instance = instance_for(epfd);
        if (!instance) {
            pthread_mutex_unlock(&instances_lock);
            errno = EBADF;
            return -1;
        }
        for (int i = 0; i < instance->watch_count && ready < maxevents; i++) {
            EpollWatch *watch = &instance->watches[i];
            if (watch->disarmed) continue;
            uint32_t live = 0;
            if (!android_eventfd_poll_readiness(watch->fd, &live)) continue;
            const uint32_t report = live &
                (watch->events | EPOLLERR_V | EPOLLHUP_V);
            if (!report) continue;
            out_events[ready] = report;
            out_data[ready] = watch->data;
            ready++;
            if (watch->events & EPOLLONESHOT_V) watch->disarmed = true;
        }
        pthread_mutex_unlock(&instances_lock);
        if (ready > 0 || timeout_millis == 0) return ready;
        const uint64_t generation =
            android_virtual_wait_change(since, timeout_millis);
        if (generation != since) {
            since = generation;
            continue;
        }
        return 0; // Waited the full timeout with no change: timed out.
    }
}

int android_epoll_close(int fd)
{
    pthread_mutex_lock(&instances_lock);
    VirtualEpoll *instance = instance_for(fd);
    if (!instance) {
        pthread_mutex_unlock(&instances_lock);
        errno = EBADF;
        return -1;
    }
    instance->in_use = false;
    instance->watch_count = 0;
    pthread_mutex_unlock(&instances_lock);
    // Wake waiters so a blocked epoll_wait rescans and fails EBADF rather
    // than sleeping past the death of its instance.
    android_virtual_note_change();
    return 0;
}
