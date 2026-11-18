#define _POSIX_C_SOURCE 200809L
#include "android_epoll_compat.h"

#include <errno.h>
#include <pthread.h>
#include <time.h>

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
//     EPOLLONESHOT, EPOLLET; edge-triggered watches report only on a
//     not-ready -> ready transition of the watched set, per Linux;
//   - readiness is recomputed from the live objects on every wake, so a
//     descriptor closed or drained by another thread is never reported
//     from stale state; a watch on a closed descriptor goes silent (the
//     Linux file-description-teardown behavior) and DEL still finds it.

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
#define EPOLL_FD_BASE 0x3FFE0000 // Clear of the eventfd generation space.
#define EPOLL_FD_GENERATIONS 1024

// Instance fds embed a reuse generation exactly like eventfd fds: a stale
// epoll fd or a blocked epoll_wait on a closed instance can never alias a
// new instance that reused the slot.

typedef struct {
    int fd;
    uint32_t events;
    uint64_t data;
    bool disarmed; // EPOLLONESHOT consumed; rearms on MOD
    bool was_ready; // EPOLLET: last scan's observed readiness
    uint64_t seen_read_edges;  // EPOLLET: transitions seen at last scan
    uint64_t seen_write_edges; // (edges can fall between two scans)
} EpollWatch;

typedef struct {
    bool in_use;
    int current_fd;
    EpollWatch watches[EPOLL_MAX_WATCHES];
    int watch_count;
} VirtualEpoll;

static VirtualEpoll instances[EPOLL_MAX_INSTANCES];
static pthread_mutex_t instances_lock = PTHREAD_MUTEX_INITIALIZER;

static VirtualEpoll *instance_for(int epfd)
{
    const int relative = epfd - EPOLL_FD_BASE;
    if (relative < 0 || relative >= EPOLL_MAX_INSTANCES * EPOLL_FD_GENERATIONS)
        return NULL;
    VirtualEpoll *instance = &instances[relative % EPOLL_MAX_INSTANCES];
    return (instance->in_use && instance->current_fd == epfd) ? instance : NULL;
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
    int generation = 0;
    if (instances[slot].current_fd)
        generation = (instances[slot].current_fd - EPOLL_FD_BASE) /
                     EPOLL_MAX_INSTANCES + 1;
    if (generation >= EPOLL_FD_GENERATIONS) {
        pthread_mutex_unlock(&instances_lock);
        errno = EMFILE;
        return -1;
    }
    instances[slot].in_use = true;
    instances[slot].watch_count = 0;
    instances[slot].current_fd = EPOLL_FD_BASE +
                                 generation * EPOLL_MAX_INSTANCES + slot;
    const int fd = instances[slot].current_fd;
    pthread_mutex_unlock(&instances_lock);
    return fd;
}

bool android_epoll_is_virtual(int fd)
{
    const int relative = fd - EPOLL_FD_BASE;
    return relative >= 0 &&
           relative < EPOLL_MAX_INSTANCES * EPOLL_FD_GENERATIONS;
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
        // ET starts from the CURRENT readiness: adding a watch for an
        // already-ready fd must report once (Linux delivers the initial
        // state as an edge), so seed the edge state as unseen.
        watch->was_ready = false;
        watch->seen_read_edges = 0;
        watch->seen_write_edges = 0;
    } else if (operation == 3) { // MOD
        if (found < 0) {
            pthread_mutex_unlock(&instances_lock);
            errno = ENOENT;
            return -1;
        }
        instance->watches[found].events = events;
        instance->watches[found].data = data;
        instance->watches[found].disarmed = false;
        // Linux treats MOD as a re-registration: an already-ready fd
        // reports again. Resetting the edge state reproduces that.
        instance->watches[found].was_ready = false;
        instance->watches[found].seen_read_edges = 0;
        instance->watches[found].seen_write_edges = 0;
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
    // The timeout budget is measured from call start: unrelated change
    // notifications must not extend the wait the way a naive re-arm would.
    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);
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
            uint64_t read_edges = 0, write_edges = 0;
            if (!android_eventfd_readiness_edges(watch->fd, &live,
                                                 &read_edges,
                                                 &write_edges)) {
                // Watched object closed: with the description gone, Linux
                // removes the interest silently; matching that, the watch
                // simply never reports again (DEL still finds it).
                watch->was_ready = false;
                continue;
            }
            const uint32_t current = live &
                (watch->events | EPOLLERR_V | EPOLLHUP_V);
            if (watch->events & EPOLLET_V) {
                // Genuine edge semantics: report when the watched set went
                // not-ready -> ready. Transitions can fall entirely
                // between two scans (drained and refilled), so the
                // object's own transition counters arbitrate, not only
                // the last scanned state.
                const bool now_ready = current != 0;
                const bool new_edge =
                    (read_edges != watch->seen_read_edges) ||
                    (write_edges != watch->seen_write_edges);
                if (now_ready && (!watch->was_ready || new_edge)) {
                    out_events[ready] = current;
                    out_data[ready] = watch->data;
                    ready++;
                    if (watch->events & EPOLLONESHOT_V)
                        watch->disarmed = true;
                }
                watch->was_ready = now_ready;
                watch->seen_read_edges = read_edges;
                watch->seen_write_edges = write_edges;
            } else if (current) {
                out_events[ready] = current;
                out_data[ready] = watch->data;
                ready++;
                if (watch->events & EPOLLONESHOT_V) watch->disarmed = true;
            }
        }
        pthread_mutex_unlock(&instances_lock);
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
