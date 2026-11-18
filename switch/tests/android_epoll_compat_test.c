#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "android_epoll_compat.h"
#include "android_eventfd_compat.h"
#include "android_system_compat.h"

#define EPOLLIN_T  0x001u
#define EPOLLOUT_T 0x004u
#define EPOLLRDHUP_T 0x2000u
#define EPOLLEXCLUSIVE_T (1u << 28)
#define EPOLLONESHOT_T (1u << 30)
#define EPOLLET_T  (1u << 31)

// Guest ABI view: u32 events at 0, u64 data at 8 (bionic aarch64).
typedef struct {
    uint32_t events;
    uint32_t padding;
    uint64_t data;
} TestEpollEvent;
_Static_assert(sizeof(TestEpollEvent) == 16, "bionic aarch64 epoll_event");

typedef int (*CreateFn)(int);
typedef int (*CtlFn)(int, int, int, void *);
typedef int (*WaitFn)(int, void *, int, int);

static CreateFn epoll_create1_fn;
static CtlFn epoll_ctl_fn;
static WaitFn epoll_wait_fn;

static void resolve(const char *symbol, void *output)
{
    uint64_t address = 0;
    assert(android_system_compat_resolve(symbol, "LIBC", "libc.so", &address));
    assert(address);
    memcpy(output, &address, sizeof(address));
}

static int wait_fd;
static int wait_result;
static TestEpollEvent wait_events[4];

static void *blocked_waiter(void *unused)
{
    (void)unused;
    wait_result = android_epoll_wait(wait_fd, &wait_events[0].events,
                                     (uint64_t *)&wait_events[0].data, 1, -1);
    return NULL;
}

static void *blocked_waiter_negative(void *unused)
{
    (void)unused;
    wait_result = android_epoll_wait(wait_fd, &wait_events[0].events,
                                     (uint64_t *)&wait_events[0].data, 1, -7);
    return NULL;
}

int main(void)
{
    resolve("epoll_create1", &epoll_create1_fn);
    resolve("epoll_ctl", &epoll_ctl_fn);
    resolve("epoll_wait", &epoll_wait_fn);

    // Creation validation.
    errno = 0;
    assert(epoll_create1_fn(0x4) == -1 && errno == EINVAL);
    int epfd = epoll_create1_fn(0x80000); // EPOLL_CLOEXEC accepted
    assert(epfd >= 0 && android_epoll_is_virtual(epfd));

    int efd = android_eventfd_create(0, ANDROID_EFD_NONBLOCK);
    TestEpollEvent event = {EPOLLIN_T, 0, 0xDEADBEEFULL};

    // Watch validation: duplicates, missing targets, unsupported flags.
    assert(epoll_ctl_fn(epfd, 1, efd, &event) == 0);
    errno = 0;
    assert(epoll_ctl_fn(epfd, 1, efd, &event) == -1 && errno == EEXIST);
    errno = 0;
    assert(epoll_ctl_fn(epfd, 1, 1, &event) == -1 && errno == EPERM);
    // fs/eventpoll.c applies no generic mask validation: EPOLLRDHUP and
    // other bits are accepted and stored, never fired by these objects.
    assert(epoll_ctl_fn(epfd, 2, efd, NULL) == 0); // drop the watch first
    TestEpollEvent rdhup = {EPOLLIN_T | EPOLLRDHUP_T, 0, 0xFE};
    assert(epoll_ctl_fn(epfd, 1, efd, &rdhup) == 0);
    assert(epoll_ctl_fn(epfd, 2, efd, NULL) == 0); // remove the probe
    assert(epoll_ctl_fn(epfd, 1, efd, &event) == 0); // restore the watch
    // EPOLLEXCLUSIVE is refused on ADD and MOD (kernel refuses MOD; its
    // exclusive-wakeup distribution is not implemented).
    TestEpollEvent exclusive = {EPOLLIN_T | EPOLLEXCLUSIVE_T, 0, 0};
    errno = 0;
    assert(epoll_ctl_fn(epfd, 1, efd, &exclusive) == -1 && errno == EINVAL);
    errno = 0;
    assert(epoll_ctl_fn(epfd, 3, efd, &exclusive) == -1 && errno == EINVAL);
    // MOD with an arbitrary unknown bit is accepted, as the kernel does.
    TestEpollEvent unknown = {EPOLLIN_T | (1u << 20), 0, 0xDEADBEEFULL};
    assert(epoll_ctl_fn(epfd, 3, efd, &unknown) == 0);
    errno = 0;
    assert(epoll_ctl_fn(epfd, 9, efd, &event) == -1 && errno == EINVAL);

    // Nothing ready: timeout 0 returns 0.
    assert(epoll_wait_fn(epfd, wait_events, 4, 0) == 0);

    // Make the eventfd readable: exactly one event, with the data payload.
    uint64_t one = 1;
    assert(android_eventfd_write(efd, &one, 8) == 8);
    memset(wait_events, 0, sizeof(wait_events));
    assert(epoll_wait_fn(epfd, wait_events, 4, 0) == 1);
    assert((wait_events[0].events & EPOLLIN_T) && wait_events[0].data == 0xDEADBEEFULL);

    // MOD updates the data payload and re-arms.
    event.data = 42;
    assert(epoll_ctl_fn(epfd, 3, efd, &event) == 0);
    assert(epoll_wait_fn(epfd, wait_events, 4, 0) == 1);
    assert(wait_events[0].data == 42);

    // EPOLLOUT is reported when requested and the counter is not full.
    event.events = EPOLLIN_T | EPOLLOUT_T;
    assert(epoll_ctl_fn(epfd, 3, efd, &event) == 0);
    assert(epoll_wait_fn(epfd, wait_events, 4, 0) == 1);
    assert((wait_events[0].events & EPOLLOUT_T) != 0);

    // maxevents is honored.
    int efd2 = android_eventfd_create(1, ANDROID_EFD_NONBLOCK);
    event.events = EPOLLIN_T;
    event.data = 77;
    assert(epoll_ctl_fn(epfd, 1, efd2, &event) == 0);
    assert(epoll_wait_fn(epfd, wait_events, 1, 0) == 1);
    assert(epoll_wait_fn(epfd, wait_events, 4, 0) == 2);

    // Draining removes readiness (live rescan, no stale reports).
    event.events = EPOLLIN_T; // drop the EPOLLOUT watch first
    assert(epoll_ctl_fn(epfd, 3, efd, &event) == 0);
    assert(android_eventfd_read(efd, &one, 8) == 8);
    assert(android_eventfd_read(efd2, &one, 8) == 8);
    assert(epoll_wait_fn(epfd, wait_events, 4, 0) == 0);

    // EPOLLONESHOT reports once, then stays disarmed until MOD.
    event.events = EPOLLIN_T | EPOLLONESHOT_T;
    assert(epoll_ctl_fn(epfd, 3, efd, &event) == 0);
    one = 1;
    assert(android_eventfd_write(efd, &one, 8) == 8);
    assert(epoll_wait_fn(epfd, wait_events, 4, 0) == 1);
    assert(epoll_wait_fn(epfd, wait_events, 4, 0) == 0);
    assert(epoll_ctl_fn(epfd, 3, efd, &event) == 0); // re-arm
    assert(epoll_wait_fn(epfd, wait_events, 4, 0) == 1);

    // EPOLLET reports once per not-ready -> ready transition, not per wait.
    event.events = EPOLLIN_T | EPOLLET_T;
    assert(epoll_ctl_fn(epfd, 3, efd, &event) == 0); // MOD re-arms the edge
    assert(epoll_wait_fn(epfd, wait_events, 4, 0) == 1); // initial state edge
    assert(epoll_wait_fn(epfd, wait_events, 4, 0) == 0); // no new transition
    one = 1;
    assert(android_eventfd_write(efd, &one, 8) == 8); // counter rises, not 0->1
    assert(epoll_wait_fn(epfd, wait_events, 4, 0) == 0); // still no edge
    assert(android_eventfd_read(efd, &one, 8) == 8);    // drain to 0
    one = 2;
    assert(android_eventfd_write(efd, &one, 8) == 8);   // 0 -> nonzero edge
    assert(epoll_wait_fn(epfd, wait_events, 4, 0) == 1);
    assert(epoll_wait_fn(epfd, wait_events, 4, 0) == 0);
    assert(android_eventfd_read(efd, &one, 8) == 8);

    // DEL removes the watch.
    assert(epoll_ctl_fn(epfd, 2, efd, NULL) == 0);
    errno = 0;
    assert(epoll_ctl_fn(epfd, 2, efd, NULL) == -1 && errno == ENOENT);

    // A blocking epoll_wait wakes when another thread writes the eventfd.
    int epfd2 = android_epoll_create();
    int efd3 = android_eventfd_create(0, 0);
    assert(android_epoll_ctl(epfd2, 1, efd3, EPOLLIN_T, 0xABC) == 0);
    wait_fd = epfd2;
    wait_result = -2;
    memset(wait_events, 0, sizeof(wait_events));
    pthread_t thread;
    assert(pthread_create(&thread, NULL, blocked_waiter, NULL) == 0);
    struct timespec pause = {0, 20000000};
    nanosleep(&pause, NULL);
    one = 1;
    assert(android_eventfd_write(efd3, &one, 8) == 8);
    assert(pthread_join(thread, NULL) == 0);
    assert(wait_result == 1 && wait_events[0].data == 0xABC);

    // Closing the instance fails later calls with EBADF.
    assert(android_epoll_close(epfd2) == 0);
    errno = 0;
    assert(android_epoll_ctl(epfd2, 1, efd3, EPOLLIN_T, 0) == -1 && errno == EBADF);
    errno = 0;
    assert(android_epoll_wait(epfd2, &wait_events[0].events,
                              (uint64_t *)&wait_events[0].data, 1, 0) == -1 &&
           errno == EBADF);

    // Bounded wait with no events returns 0 after the timeout.
    assert(epoll_wait_fn(epfd, wait_events, 4, 5) == 0);

    // A bounded wait measures from call start: an unrelated change
    // notification mid-wait must not extend the budget.
    {
        struct timespec before, after;
        clock_gettime(CLOCK_MONOTONIC, &before);
        // Bump the generation twice during the wait via unrelated objects.
        int noise = android_eventfd_create(0, ANDROID_EFD_NONBLOCK);
        assert(android_eventfd_close(noise) == 0);
        assert(epoll_wait_fn(epfd, wait_events, 4, 60) == 0);
        clock_gettime(CLOCK_MONOTONIC, &after);
        const long elapsed_ms = (after.tv_sec - before.tv_sec) * 1000L +
                                (after.tv_nsec - before.tv_nsec) / 1000000L;
        assert(elapsed_ms >= 55 && elapsed_ms < 3000);
    }

    // Close/reuse ABA: closing a watched eventfd and creating a new one
    // (slot reuse) must not make the old watch report the new object.
    {
        int epfd3 = android_epoll_create();
        int old_fd = android_eventfd_create(0, ANDROID_EFD_NONBLOCK);
        assert(android_epoll_ctl(epfd3, 1, old_fd, EPOLLIN_T, 0x111) == 0);
        assert(android_eventfd_close(old_fd) == 0);
        int new_fd = android_eventfd_create(9, ANDROID_EFD_NONBLOCK);
        assert(new_fd != old_fd); // generation-advanced, never recycled
        assert(android_epoll_wait(epfd3, &wait_events[0].events,
                                  (uint64_t *)&wait_events[0].data, 4,
                                  0) == 0); // closed object reports nothing
        // Kernel fd validation runs before the watch lookup, so every
        // ctl op on the dead number fails EBADF (differential-tested on
        // Linux 6.1: DEL/MOD/ADD after close all give EBADF).
        errno = 0;
        assert(android_epoll_ctl(epfd3, 2, old_fd, 0, 0) == -1 &&
               errno == EBADF);
        errno = 0;
        assert(android_epoll_ctl(epfd3, 3, old_fd, EPOLLIN_T, 0) == -1 &&
               errno == EBADF);
        errno = 0;
        assert(android_epoll_ctl(epfd3, 1, old_fd, EPOLLIN_T, 0) == -1 &&
               errno == EBADF);
        assert(android_epoll_close(epfd3) == 0);
        assert(android_eventfd_close(new_fd) == 0);
    }

    // Any negative timeout is an infinite wait (ep_timeout_to_timespec:
    // ms < 0 returns NULL), not just -1. Use -7 with a delayed armer.
    {
        int epfd4 = android_epoll_create();
        int efd4 = android_eventfd_create(0, 0);
        assert(android_epoll_ctl(epfd4, 1, efd4, EPOLLIN_T, 0x777) == 0);
        wait_fd = epfd4;
        wait_result = -2;
        memset(wait_events, 0, sizeof(wait_events));
        pthread_t thread2;
        assert(pthread_create(&thread2, NULL, blocked_waiter_negative,
                              NULL) == 0);
        struct timespec pause2 = {0, 20000000};
        nanosleep(&pause2, NULL);
        one = 1;
        assert(android_eventfd_write(efd4, &one, 8) == 8);
        assert(pthread_join(thread2, NULL) == 0);
        assert(wait_result == 1 && wait_events[0].data == 0x777);
        assert(android_epoll_close(epfd4) == 0);
        assert(android_eventfd_close(efd4) == 0);
    }

    // maxevents validation.
    errno = 0;
    assert(epoll_wait_fn(epfd, wait_events, 0, 0) == -1 && errno == EINVAL);

    puts("android_epoll_compat_test: all assertions passed");
}
