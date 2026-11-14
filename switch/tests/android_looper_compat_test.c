#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "android_looper_compat.h"

typedef void *(*Prepare)(int);
typedef void *(*ForThread)(void);
typedef void (*Ref)(void *);
typedef int (*Callback)(int, int, void *);
typedef int (*AddFd)(void *, int, int, int, Callback, void *);
typedef int (*RemoveFd)(void *, int);
typedef int (*PollOnce)(int, int *, int *, void **);

static Prepare prepare;
static ForThread for_thread;
static Ref acquire;
static Ref release;
static AddFd add_fd;
static RemoveFd remove_fd;
static PollOnce poll_once;
static unsigned callbacks;

static void function(const char *symbol, void *output)
{
    uint64_t address = 0;
    assert(android_looper_compat_resolve(symbol, NULL, NULL, &address));
    assert(address);
    memcpy(output, &address, sizeof(address));
}

static int on_fd(int fd, int events, void *data)
{
    assert(events & 1);
    assert(data == &callbacks);
    char ch;
    assert(read(fd, &ch, 1) == 1);
    callbacks++;
    return 0; // Android callbacks returning zero auto-unregister.
}

static void *thread_test(void *unused)
{
    (void)unused;
    assert(for_thread() == NULL);
    assert(poll_once(0, NULL, NULL, NULL) == -4);
    void *looper = prepare(0);
    assert(looper && for_thread() == looper);
    int fds[2];
    assert(pipe(fds) == 0);
    assert(add_fd(looper, fds[0], 55, 1, NULL, NULL) == -1);
    assert(add_fd(looper, fds[0], 55, 1, on_fd, &callbacks) == 1);
    assert(remove_fd(looper, fds[0]) == 1);
    close(fds[0]); close(fds[1]);
    return NULL; // pthread_key destructor drops implicit thread ownership.
}

int main(void)
{
    function("ALooper_prepare", &prepare);
    function("ALooper_forThread", &for_thread);
    function("ALooper_acquire", &acquire);
    function("ALooper_release", &release);
    function("ALooper_addFd", &add_fd);
    function("ALooper_removeFd", &remove_fd);
    function("ALooper_pollOnce", &poll_once);
    uint64_t bad = 0;
    assert(!android_looper_compat_resolve("ALooper_pollOnce", "LIBC", "libc.so", &bad));
    assert(!android_looper_compat_resolve("ALooper_fake", NULL, NULL, &bad));
    assert(for_thread() == NULL);
    assert(poll_once(0, NULL, NULL, NULL) == -4);

    void *looper = prepare(1);
    assert(looper && for_thread() == looper);
    assert(prepare(1) == looper);
    acquire(looper);
    release(looper);
    assert(poll_once(0, NULL, NULL, NULL) == -3);
    assert(add_fd(looper, -1, 42, 1, NULL, NULL) == -1);
    assert(add_fd(looper, 0, 42, 8, NULL, NULL) == -1);

    int a[2], b[2], c[2];
    assert(pipe(a) == 0 && pipe(b) == 0 && pipe(c) == 0);
    assert(add_fd(looper, a[0], 0, 1, on_fd, &callbacks) == 1);
    assert(add_fd(looper, b[0], 0, 1, on_fd, &callbacks) == 1);
    assert(write(a[1], "a", 1) == 1 && write(b[1], "b", 1) == 1);
    assert(poll_once(0, NULL, NULL, NULL) == -2);
    assert(callbacks == 2); // Both callbacks in one ready batch.
    assert(remove_fd(looper, a[0]) == 0 && remove_fd(looper, b[0]) == 0);

    assert(add_fd(looper, c[0], 77, 1, NULL, &callbacks) == 1);
    assert(write(c[1], "x", 1) == 1);
    int result_fd = -1, result_events = -1;
    void *result_data = NULL;
    assert(poll_once(25, &result_fd, &result_events, &result_data) == 77);
    assert(result_fd == c[0] && (result_events & 1));
    assert(result_data == &callbacks);
    assert(remove_fd(looper, c[0]) == 1);
    assert(poll_once(0, NULL, NULL, NULL) == -3);

    close(a[0]); close(a[1]); close(b[0]); close(b[1]);
    close(c[0]); close(c[1]);
    pthread_t thread;
    assert(pthread_create(&thread, NULL, thread_test, NULL) == 0);
    assert(pthread_join(thread, NULL) == 0);
    assert(for_thread() == looper);
    puts("android_looper_compat_test: all assertions passed");
}
