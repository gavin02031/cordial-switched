#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "android_eventfd_compat.h"
#include "android_libc_compat.h"

// Linux eventfd semantics are the contract; bionic exposes them unchanged.

typedef ssize_t (*IoFn)(int, void *, size_t);
typedef int (*CloseFn)(int);

static IoFn routed_read;
static IoFn routed_write;
static CloseFn routed_close;

static void route(const char *symbol, void *output)
{
    uint64_t address = 0;
    assert(android_libc_compat_resolve(symbol, "LIBC", "libc.so", &address));
    assert(address);
    memcpy(output, &address, sizeof(address));
}

static int blocked_fd;
static uint64_t blocked_value;
static ssize_t blocked_result;

static void *blocked_reader(void *unused)
{
    (void)unused;
    blocked_result = android_eventfd_read(blocked_fd, &blocked_value, 8);
    return NULL;
}

int main(void)
{
    route("read", &routed_read);
    route("write", &routed_write);
    route("close", &routed_close);

    // Flag validation.
    errno = 0;
    assert(android_eventfd_create(0, 0x4) == -1 && errno == EINVAL);
    errno = 0;
    assert(android_eventfd_create(0, ANDROID_EFD_SEMAPHORE |
           ANDROID_EFD_NONBLOCK | ANDROID_EFD_CLOEXEC) >= 0);

    // Nonblocking empty read fails EAGAIN.
    int fd = android_eventfd_create(0, ANDROID_EFD_NONBLOCK);
    assert(fd >= 0 && android_eventfd_is_virtual(fd));
    uint64_t value = 0xEE;
    errno = 0;
    assert(android_eventfd_read(fd, &value, 8) == -1 && errno == EAGAIN);
    assert(value == 0xEE); // Failed reads do not touch the buffer.

    // Buffer-size and value validation.
    errno = 0;
    assert(android_eventfd_read(fd, &value, 4) == -1 && errno == EINVAL);
    value = UINT64_MAX;
    errno = 0;
    assert(android_eventfd_write(fd, &value, 8) == -1 && errno == EINVAL);

    // Plain mode drains the whole counter.
    value = 5;
    assert(android_eventfd_write(fd, &value, 8) == 8);
    value = 0;
    assert(android_eventfd_read(fd, &value, 8) == 8 && value == 5);
    errno = 0;
    assert(android_eventfd_read(fd, &value, 8) == -1 && errno == EAGAIN);

    // Writing zero is legal and changes nothing.
    value = 0;
    assert(android_eventfd_write(fd, &value, 8) == 8);
    errno = 0;
    assert(android_eventfd_read(fd, &value, 8) == -1 && errno == EAGAIN);
    assert(routed_close(fd) == 0);
    errno = 0;
    assert(android_eventfd_read(fd, &value, 8) == -1 && errno == EBADF);

    // Semaphore mode drains exactly one per read.
    fd = android_eventfd_create(0, ANDROID_EFD_SEMAPHORE |
                                ANDROID_EFD_NONBLOCK);
    value = 3;
    assert(android_eventfd_write(fd, &value, 8) == 8);
    for (int i = 0; i < 3; i++) {
        value = 0;
        assert(android_eventfd_read(fd, &value, 8) == 8 && value == 1);
    }
    errno = 0;
    assert(android_eventfd_read(fd, &value, 8) == -1 && errno == EAGAIN);
    assert(android_eventfd_close(fd) == 0);

    // Saturation: the counter may not pass 2^64 - 2.
    fd = android_eventfd_create(0, ANDROID_EFD_NONBLOCK);
    value = 0xFFFFFFFFFFFFFFFEULL;
    assert(android_eventfd_write(fd, &value, 8) == 8);
    value = 1;
    errno = 0;
    assert(android_eventfd_write(fd, &value, 8) == -1 && errno == EAGAIN);
    assert(android_eventfd_close(fd) == 0);

    // Blocking read wakes on a later write from another thread.
    fd = android_eventfd_create(0, 0);
    blocked_fd = fd;
    blocked_result = -2;
    blocked_value = 0;
    pthread_t thread;
    assert(pthread_create(&thread, NULL, blocked_reader, NULL) == 0);
    struct timespec pause = {0, 20000000};
    nanosleep(&pause, NULL); // Let the reader block first.
    value = 7;
    assert(android_eventfd_write(fd, &value, 8) == 8);
    assert(pthread_join(thread, NULL) == 0);
    assert(blocked_result == 8 && blocked_value == 7);
    assert(android_eventfd_close(fd) == 0);

    // End-to-end routing: guest read/write/close resolve to the virtual
    // object; host fds (here: stdout) fall through untouched.
    fd = android_eventfd_create(9, ANDROID_EFD_NONBLOCK);
    value = 0;
    assert(routed_read(fd, &value, 8) == 8 && value == 9);
    value = 4;
    assert(routed_write(fd, &value, 8) == 8);
    value = 0;
    assert(routed_read(fd, &value, 8) == 8 && value == 4);
    assert(routed_close(fd) == 0);
    assert(routed_write(1, " ", 1) == 1); // Host fd passthrough.
    assert(!android_eventfd_is_virtual(1));

    puts("android_eventfd_compat_test: all assertions passed");
}
