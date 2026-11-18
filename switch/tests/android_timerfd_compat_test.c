#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "android_epoll_compat.h"
#include "android_libc_compat.h"
#include "android_system_compat.h"
#include "android_timerfd_compat.h"

typedef ssize_t (*IoFn)(int, void *, size_t);
typedef int (*CloseFn)(int);
typedef int (*TimerfdCreateFn)(int, int);
typedef int (*TimerfdSettimeFn)(int, int, const void *, void *);

static IoFn routed_read;
static CloseFn routed_close;
static TimerfdCreateFn create_fn;
static TimerfdSettimeFn settime_fn;

static void route_libc(const char *symbol, void *output)
{
    uint64_t address = 0;
    assert(android_libc_compat_resolve(symbol, "LIBC", "libc.so", &address));
    assert(address);
    memcpy(output, &address, sizeof(address));
}

static void route_system(const char *symbol, void *output)
{
    uint64_t address = 0;
    assert(android_system_compat_resolve(symbol, "LIBC", "libc.so", &address));
    assert(address);
    memcpy(output, &address, sizeof(address));
}

static void sleep_ms(long ms)
{
    struct timespec pause = {ms / 1000, (ms % 1000) * 1000000L};
    nanosleep(&pause, NULL);
}

static long monotonic_ms(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return now.tv_sec * 1000L + now.tv_nsec / 1000000;
}

int main(void)
{
    route_libc("read", &routed_read);
    route_libc("close", &routed_close);
    route_system("timerfd_create", &create_fn);
    route_system("timerfd_settime", &settime_fn);

    // Clock and flag validation.
    errno = 0;
    assert(create_fn(7, 0) == -1 && errno == EINVAL);
    errno = 0;
    assert(create_fn(1, 0x4) == -1 && errno == EINVAL);

    int fd = create_fn(1, ANDROID_TFD_NONBLOCK); // CLOCK_MONOTONIC
    assert(fd >= 0 && android_timerfd_is_virtual(fd));

    // Read before any arm: EAGAIN when nonblocking.
    uint64_t value = 0xEE;
    errno = 0;
    assert(android_timerfd_read(fd, &value, 8) == -1 && errno == EAGAIN);

    // Bad specs rejected exactly as Linux does.
    AndroidItimerspec spec = {0, 0, 0, 30LL * 1000000LL}; // 30 ms one-shot
    errno = 0;
    assert(settime_fn(fd, 2, &spec, NULL) == -1 && errno == EINVAL);
    spec.value_nsec = 1000000000LL;
    errno = 0;
    assert(settime_fn(fd, 0, &spec, NULL) == -1 && errno == EINVAL);

    // One-shot, relative: fires once, then disarms.
    spec.value_nsec = 30LL * 1000000LL;
    spec.interval_sec = 0;
    spec.interval_nsec = 0;
    assert(settime_fn(fd, 0, &spec, NULL) == 0);
    sleep_ms(60);
    value = 0;
    assert(routed_read(fd, &value, 8) == 8 && value == 1);
    errno = 0;
    assert(routed_read(fd, &value, 8) == -1 && errno == EAGAIN);

    // Periodic 20 ms: after ~65 ms at least 3 expirations accumulate.
    spec.value_sec = 0;
    spec.value_nsec = 20LL * 1000000LL;
    spec.interval_nsec = 20LL * 1000000LL;
    AndroidItimerspec old = {-1, -1, -1, -1};
    assert(settime_fn(fd, 0, &spec, &old) == 0);
    assert(old.value_sec == 0 && old.value_nsec == 0); // was disarmed
    sleep_ms(65);
    value = 0;
    assert(android_timerfd_read(fd, &value, 8) == 8 && value >= 3);

    // Disarm: pending delivered count survives, future ones stop.
    memset(&spec, 0, sizeof(spec));
    assert(settime_fn(fd, 0, &spec, NULL) == 0);
    errno = 0;
    assert(android_timerfd_read(fd, &value, 8) == -1 && errno == EAGAIN);

    // Blocking read wakes at expiry without any signal.
    fd = create_fn(1, 0);
    spec.value_sec = 0;
    spec.value_nsec = 40LL * 1000000LL;
    spec.interval_sec = 0;
    spec.interval_nsec = 0;
    assert(settime_fn(fd, 0, &spec, NULL) == 0);
    const long before = monotonic_ms();
    value = 0;
    assert(android_timerfd_read(fd, &value, 8) == 8 && value == 1);
    const long waited = monotonic_ms() - before;
    assert(waited >= 35 && waited < 3000);

    // epoll over a timerfd: level readiness plus a wake at expiry.
    int epfd = android_epoll_create();
    int tfd = create_fn(1, 0);
    assert(android_epoll_ctl(epfd, 1, tfd, 0x001, 0x5A) == 0);
    uint32_t out_events[2];
    uint64_t out_data[2];
    assert(android_epoll_wait(epfd, out_events, out_data, 2, 0) == 0);
    spec.value_sec = 0;
    spec.value_nsec = 40LL * 1000000LL;
    assert(android_timerfd_settime(tfd, 0, &spec, NULL) == 0);
    const long ep_before = monotonic_ms();
    assert(android_epoll_wait(epfd, out_events, out_data, 2, -1) == 1);
    const long ep_waited = monotonic_ms() - ep_before;
    assert(ep_waited >= 35 && ep_waited < 3000);
    assert((out_events[0] & 0x001) && out_data[0] == 0x5A);

    // EPOLLET on a timerfd: one report per firing, not per wait.
    uint64_t drained = 0;
    assert(android_timerfd_read(tfd, &drained, 8) == 8);
    assert(android_epoll_ctl(epfd, 3, tfd, 0x001 | (1u << 31), 0x5B) == 0);
    spec.value_sec = 0;
    spec.value_nsec = 25LL * 1000000LL;
    spec.interval_nsec = 25LL * 1000000LL; // periodic for a second edge
    assert(android_timerfd_settime(tfd, 0, &spec, NULL) == 0);
    sleep_ms(35);
    assert(android_epoll_wait(epfd, out_events, out_data, 2, 0) == 1);
    assert(android_epoll_wait(epfd, out_events, out_data, 2, 0) == 0);
    sleep_ms(30); // another period fires: a fresh edge
    assert(android_epoll_wait(epfd, out_events, out_data, 2, 0) == 1);
    assert(android_epoll_close(epfd) == 0);
    assert(routed_close(tfd) == 0);

    // Routing: write on a timerfd fails EINVAL like Linux.
    value = 1;
    ssize_t (*routed_write)(int, const void *, size_t);
    route_libc("write", &routed_write);
    errno = 0;
    assert(routed_write(fd, &value, 8) == -1 && errno == EINVAL);
    assert(routed_close(fd) == 0);
    errno = 0;
    assert(android_timerfd_read(fd, &value, 8) == -1 && errno == EBADF);

    puts("android_timerfd_compat_test: all assertions passed");
}
