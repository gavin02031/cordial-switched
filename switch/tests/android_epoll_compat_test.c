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
    errno = 0;
    TestEpollEvent bad = {0x8000u, 0, 0}; // EPOLLRDHUP: outside the subset
    assert(epoll_ctl_fn(epfd, 1, efd, &bad) == -1 && errno == EINVAL);
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

    // EPOLLET is accepted (reported level-triggered, documented deviation).
    event.events = EPOLLIN_T | EPOLLET_T;
    assert(epoll_ctl_fn(epfd, 3, efd, &event) == 0);
    assert(epoll_wait_fn(epfd, wait_events, 4, 0) == 1);

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

    // maxevents validation.
    errno = 0;
    assert(epoll_wait_fn(epfd, wait_events, 0, 0) == -1 && errno == EINVAL);

    puts("android_epoll_compat_test: all assertions passed");
}
