#define _POSIX_C_SOURCE 200809L
#include "android_eventfd_compat.h"

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <string.h>

// Semantics follow the Linux eventfd(2) contract exactly, because bionic
// exposes it unchanged and the guest's code was written against it:
//   - the counter saturates at 2^64 - 2; a write that would exceed it
//     fails EAGAIN (nonblocking) or blocks;
//   - a read on a zero counter fails EAGAIN (nonblocking) or blocks;
//   - EFD_SEMAPHORE reads drain exactly 1; plain reads drain the counter;
//   - read/write buffers must be 8 bytes; the value 2^64 - 1 is invalid.
// Blocking waits use pthread conditions, which libgloss/libsysbase
// implements over Horizon condvars (devkitPro/newlib libgloss
// libsysbase/pthread.c), so the same code runs on host and Switch.

#define EVENTFD_MAX_OBJECTS 64
#define EVENTFD_FD_BASE 0x3FFD0000
#define EVENTFD_COUNTER_LIMIT UINT64_C(0xFFFFFFFFFFFFFFFE)

typedef struct {
    bool in_use;
    bool semaphore;
    bool nonblock;
    uint64_t counter;
    pthread_mutex_t lock;
    pthread_cond_t readable;
    pthread_cond_t writable;
} VirtualEventFd;

static VirtualEventFd objects[EVENTFD_MAX_OBJECTS];
static pthread_mutex_t table_lock = PTHREAD_MUTEX_INITIALIZER;
static bool table_initialized;

static void table_init(void)
{
    pthread_mutex_lock(&table_lock);
    if (!table_initialized) {
        for (int i = 0; i < EVENTFD_MAX_OBJECTS; i++) {
            pthread_mutex_init(&objects[i].lock, NULL);
            pthread_cond_init(&objects[i].readable, NULL);
            pthread_cond_init(&objects[i].writable, NULL);
        }
        table_initialized = true;
    }
    pthread_mutex_unlock(&table_lock);
}

static VirtualEventFd *object_for(int fd)
{
    const int slot = fd - EVENTFD_FD_BASE;
    if (slot < 0 || slot >= EVENTFD_MAX_OBJECTS) return NULL;
    VirtualEventFd *object = &objects[slot];
    return object->in_use ? object : NULL;
}

int android_eventfd_create(unsigned initial, int flags)
{
    if (flags & ~(ANDROID_EFD_SEMAPHORE | ANDROID_EFD_NONBLOCK |
                  ANDROID_EFD_CLOEXEC)) {
        errno = EINVAL;
        return -1;
    }
    table_init();
    pthread_mutex_lock(&table_lock);
    int slot = -1;
    for (int i = 0; i < EVENTFD_MAX_OBJECTS; i++) {
        if (!objects[i].in_use) { slot = i; break; }
    }
    if (slot < 0) {
        pthread_mutex_unlock(&table_lock);
        errno = EMFILE;
        return -1;
    }
    VirtualEventFd *object = &objects[slot];
    pthread_mutex_lock(&object->lock);
    object->in_use = true;
    object->semaphore = (flags & ANDROID_EFD_SEMAPHORE) != 0;
    object->nonblock = (flags & ANDROID_EFD_NONBLOCK) != 0;
    object->counter = initial;
    pthread_mutex_unlock(&object->lock);
    pthread_mutex_unlock(&table_lock);
    // EFD_CLOEXEC is accepted and intentionally a no-op: the guest cannot
    // exec on Horizon, so there is no exec boundary to close across.
    return EVENTFD_FD_BASE + slot;
}

bool android_eventfd_is_virtual(int fd)
{
    return fd >= EVENTFD_FD_BASE && fd < EVENTFD_FD_BASE + EVENTFD_MAX_OBJECTS;
}

ssize_t android_eventfd_read(int fd, void *buffer, size_t count)
{
    VirtualEventFd *object = object_for(fd);
    if (!object) { errno = EBADF; return -1; }
    if (count < 8) { errno = EINVAL; return -1; }
    pthread_mutex_lock(&object->lock);
    while (object->counter == 0 && object->in_use) {
        if (object->nonblock) {
            pthread_mutex_unlock(&object->lock);
            errno = EAGAIN;
            return -1;
        }
        pthread_cond_wait(&object->readable, &object->lock);
    }
    if (!object->in_use) {
        // Woken by a close while blocked: the fd is gone, not ready.
        pthread_mutex_unlock(&object->lock);
        errno = EBADF;
        return -1;
    }
    uint64_t value;
    if (object->semaphore) {
        value = 1;
        object->counter--;
    } else {
        value = object->counter;
        object->counter = 0;
    }
    pthread_cond_signal(&object->writable);
    pthread_mutex_unlock(&object->lock);
    memcpy(buffer, &value, 8);
    return 8;
}

ssize_t android_eventfd_write(int fd, const void *buffer, size_t count)
{
    VirtualEventFd *object = object_for(fd);
    if (!object) { errno = EBADF; return -1; }
    if (count < 8) { errno = EINVAL; return -1; }
    uint64_t value;
    memcpy(&value, buffer, 8);
    if (value == UINT64_MAX) { errno = EINVAL; return -1; }
    pthread_mutex_lock(&object->lock);
    while (object->counter > EVENTFD_COUNTER_LIMIT - value && object->in_use) {
        if (object->nonblock) {
            pthread_mutex_unlock(&object->lock);
            errno = EAGAIN;
            return -1;
        }
        pthread_cond_wait(&object->writable, &object->lock);
    }
    if (!object->in_use) {
        pthread_mutex_unlock(&object->lock);
        errno = EBADF;
        return -1;
    }
    object->counter += value;
    pthread_cond_signal(&object->readable);
    pthread_mutex_unlock(&object->lock);
    return 8;
}

int android_eventfd_close(int fd)
{
    VirtualEventFd *object = object_for(fd);
    if (!object) { errno = EBADF; return -1; }
    pthread_mutex_lock(&object->lock);
    object->in_use = false;
    object->counter = 0;
    // Wake every waiter: a closed fd must not leave a blocked guest thread
    // asleep on a counter that can never move again.
    pthread_cond_broadcast(&object->readable);
    pthread_cond_broadcast(&object->writable);
    pthread_mutex_unlock(&object->lock);
    return 0;
}
