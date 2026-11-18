#define _POSIX_C_SOURCE 200809L
#include "android_epoll_compat.h"

#include <errno.h>
#include <pthread.h>
#include <time.h>

#include "android_eventfd_compat.h"
#include "android_virtual_fd.h"

// Linux epoll semantics, subset. Bionic passes the ABI through unchanged
// (aarch64 struct epoll_event is 16 bytes: u32 events + 8-byte data at
// offset 8; flags are the asm-generic values verified against bionic's
// kernel uapi eventpoll.h). Validation follows fs/eventpoll.c (v6.12):
//   - the kernel applies no generic event-mask validation; unknown bits
//     (EPOLLPRI, EPOLLRDHUP, ...) are accepted, stored, and simply never
//     fire for our object kinds, so they are accepted here too;
//   - EPOLLEXCLUSIVE is refused with EINVAL on both ADD and MOD: MOD is
//     exactly the kernel's rule, and on ADD the kernel accepts it but
//     the exclusive-wakeup distribution is not implemented, so it is
//     refused explicitly rather than accepted and silently broadcast
//     (differential-tested against Linux 6.1);
//   - targets must be Morrow-virtual descriptors; a dead virtual number
//     (in-range but closed) fails EBADF exactly as Linux fd validation
//     does, and a foreign number fails EPERM, as Linux does for open
//     files without a poll method - never silently level-poll a socket
//     or file;
//   - edge-triggered watches report only on a not-ready -> ready
//     transition of the watched set, per Linux;
//   - readiness is recomputed from the live objects on every wake, so a
//     descriptor drained by another thread is never reported from stale
//     state; closing a watched descriptor drops its watches at close
//     time (Linux file-description teardown via eventpoll_release_file),
//     after which any ctl on the dead number fails EBADF, as the
//     kernel's fd validation does (differential-tested).

#define EPOLLIN_V     0x001u
#define EPOLLOUT_V    0x004u
#define EPOLLERR_V    0x008u
#define EPOLLHUP_V    0x010u
#define EPOLLEXCLUSIVE_V (1u << 28)
#define EPOLLONESHOT_V (1u << 30)
#define EPOLLET_V     (1u << 31)

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

// Linux removes a watch when the watched file description is released
// (eventpoll_release_file). Registered as the virtual-fd close hook; a
// closed object never reports again and DEL then fails ENOENT.
static void drop_watches_for(int fd)
{
    pthread_mutex_lock(&instances_lock);
    for (int s = 0; s < EPOLL_MAX_INSTANCES; s++) {
        VirtualEpoll *instance = &instances[s];
        if (!instance->in_use) continue;
        for (int i = 0; i < instance->watch_count; i++) {
            if (instance->watches[i].fd != fd) continue;
            instance->watch_count--;
            if (i < instance->watch_count)
                instance->watches[i] =
                    instance->watches[instance->watch_count];
            i--;
        }
    }
    pthread_mutex_unlock(&instances_lock);
}

int android_epoll_create(void)
{
    pthread_mutex_lock(&instances_lock);
    android_virtual_register_close_hook(drop_watches_for);
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
    // fs/eventpoll.c validates only EPOLLEXCLUSIVE: EINVAL on MOD,
    // EINVAL on ADD for combinations we also reject. Exclusive-wakeup
    // distribution is not implemented, so ADD is refused too - an
    // explicit refusal, not a silent behavior difference. Every other
    // mask bit is accepted and stored, exactly as the kernel stores it;
    // bits no object ever raises can never fire.
    if ((operation == 1 || operation == 3) &&
        (events & EPOLLEXCLUSIVE_V)) {
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
        // Kernel ordering, differential-tested on Linux 6.1: a closed
        // descriptor number fails EBADF before any watch lookup; an
        // open-but-unpollable file fails EPERM; only a live, watchable
        // target reaches the registry and can yield ENOENT.
        if (android_virtual_is_virtual(fd)) {
            if (!android_virtual_readiness_edges(fd, NULL, NULL, NULL)) {
                pthread_mutex_unlock(&instances_lock);
                errno = EBADF;
                return -1;
            }
        } else {
            pthread_mutex_unlock(&instances_lock);
            errno = EPERM;
            return -1;
        }
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
    // ADD and MOD both require a watchable target. A dead virtual
    // number is EBADF (kernel fd validation); a foreign number is EPERM
    // (kernel's open-but-unpollable rule). Refusing foreign numbers
    // keeps silent wrong readiness off the console.
    if (!android_virtual_is_virtual(fd)) {
        pthread_mutex_unlock(&instances_lock);
        errno = EPERM;
        return -1;
    }
    if (!android_virtual_readiness_edges(fd, NULL, NULL, NULL)) {
        pthread_mutex_unlock(&instances_lock);
        errno = EBADF;
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
            if (!android_virtual_readiness_edges(watch->fd, &live,
                                                 &read_edges,
                                                 &write_edges)) {
                // Watched object already closed: the close hook drops
                // watches eagerly, so this is only the narrow race of a
                // close that has not notified yet; stay silent for it.
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
        // Timer expiries are time-driven: no change signal fires for
        // them, so cap the wait slice at the soonest watched expiry and
        // re-scan when it comes due.
        int watched[EPOLL_MAX_WATCHES];
        int watched_count = 0;
        pthread_mutex_lock(&instances_lock);
        VirtualEpoll *live_instance = instance_for(epfd);
        if (live_instance) {
            watched_count = live_instance->watch_count;
            for (int i = 0; i < watched_count; i++)
                watched[i] = live_instance->watches[i].fd;
        }
        pthread_mutex_unlock(&instances_lock);
        const int64_t expiry =
            android_virtual_next_expiry_ms(watched, watched_count);
        if (expiry >= 0 && (remaining < 0 || expiry < remaining))
            remaining = (int)expiry;
        if (remaining == 0) continue; // Due now: re-scan without sleeping.
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
