#define _POSIX_C_SOURCE 200809L
#include "android_eventfd_compat.h"

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

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
#define EVENTFD_FD_GENERATIONS 1024
#define EVENTFD_COUNTER_LIMIT UINT64_C(0xFFFFFFFFFFFFFFFE)

// A virtual fd embeds a reuse generation (fd = BASE + gen*OBJECTS + slot)
// so an epoll watch or a stale caller holding a closed fd can never alias
// a NEW object that reused its slot: after close, the old number names
// nothing (watches on it simply never report, like a destroyed Linux file
// description).

typedef struct {
    bool in_use;
    bool semaphore;
    bool nonblock;
    int current_fd;
    uint64_t counter;
    uint64_t read_edges;  // 0 -> nonzero transitions (EPOLLET, EPOLLIN)
    uint64_t write_edges; // saturated -> writable transitions (EPOLLET, EPOLLOUT)
    pthread_mutex_t lock;
    pthread_cond_t readable;
    pthread_cond_t writable;
} VirtualEventFd;

static VirtualEventFd objects[EVENTFD_MAX_OBJECTS];
static pthread_mutex_t table_lock = PTHREAD_MUTEX_INITIALIZER;
static bool table_initialized;

// Shared change notification for every Morrow-virtual descriptor. Waiters
// (epoll_wait today) sleep here instead of spinning.
static pthread_mutex_t change_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t change_signal = PTHREAD_COND_INITIALIZER;
static uint64_t change_generation;

void android_virtual_note_change(void)
{
    pthread_mutex_lock(&change_lock);
    change_generation++;
    pthread_cond_broadcast(&change_signal);
    pthread_mutex_unlock(&change_lock);
}

uint64_t android_virtual_generation(void)
{
    pthread_mutex_lock(&change_lock);
    const uint64_t generation = change_generation;
    pthread_mutex_unlock(&change_lock);
    return generation;
}

uint64_t android_virtual_wait_change(uint64_t since, int timeout_millis)
{
    pthread_mutex_lock(&change_lock);
    if (timeout_millis < 0) {
        while (change_generation <= since)
            pthread_cond_wait(&change_signal, &change_lock);
    } else {
        struct timespec deadline;
        clock_gettime(CLOCK_REALTIME, &deadline);
        deadline.tv_sec += timeout_millis / 1000;
        deadline.tv_nsec += (timeout_millis % 1000) * 1000000L;
        if (deadline.tv_nsec >= 1000000000L) {
            deadline.tv_sec++;
            deadline.tv_nsec -= 1000000000L;
        }
        while (change_generation <= since) {
            if (pthread_cond_timedwait(&change_signal, &change_lock,
                                       &deadline) == ETIMEDOUT)
                break;
        }
    }
    const uint64_t generation = change_generation;
    pthread_mutex_unlock(&change_lock);
    return generation;
}

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
    const int relative = fd - EVENTFD_FD_BASE;
    if (relative < 0 ||
        relative >= EVENTFD_MAX_OBJECTS * EVENTFD_FD_GENERATIONS)
        return NULL;
    VirtualEventFd *object = &objects[relative % EVENTFD_MAX_OBJECTS];
    // Generation check: a closed-then-reused slot only answers to its
    // current fd number.
    return (object->in_use && object->current_fd == fd) ? object : NULL;
}

bool android_eventfd_readiness_edges(int fd, uint32_t *ready_events,
                                     uint64_t *read_edges,
                                     uint64_t *write_edges)
{
    VirtualEventFd *object = object_for(fd);
    if (!object) return false;
    pthread_mutex_lock(&object->lock);
    uint32_t ready = 0;
    if (object->counter > 0) ready |= 0x001;             // EPOLLIN
    if (object->counter < EVENTFD_COUNTER_LIMIT) ready |= 0x004; // EPOLLOUT
    if (ready_events) *ready_events = ready;
    if (read_edges) *read_edges = object->read_edges;
    if (write_edges) *write_edges = object->write_edges;
    pthread_mutex_unlock(&object->lock);
    return true;
}

bool android_eventfd_poll_readiness(int fd, uint32_t *ready_events)
{
    return android_eventfd_readiness_edges(fd, ready_events, NULL, NULL);
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
    object->read_edges = 0;
    object->write_edges = 0;
    int generation = 0;
    if (object->current_fd) // Slot reuse: advance the generation.
        generation = (object->current_fd - EVENTFD_FD_BASE) /
                     EVENTFD_MAX_OBJECTS + 1;
    if (generation >= EVENTFD_FD_GENERATIONS) {
        pthread_mutex_unlock(&object->lock);
        pthread_mutex_unlock(&table_lock);
        errno = EMFILE; // Slot's fd numbers exhausted; never recycle one.
        return -1;
    }
    object->current_fd = EVENTFD_FD_BASE +
                         generation * EVENTFD_MAX_OBJECTS + slot;
    const int fd = object->current_fd;
    pthread_mutex_unlock(&object->lock);
    pthread_mutex_unlock(&table_lock);
    android_virtual_note_change();
    // EFD_CLOEXEC is accepted and intentionally a no-op: the guest cannot
    // exec on Horizon, so there is no exec boundary to close across.
    return fd;
}

bool android_eventfd_is_virtual(int fd)
{
    const int relative = fd - EVENTFD_FD_BASE;
    return relative >= 0 &&
           relative < EVENTFD_MAX_OBJECTS * EVENTFD_FD_GENERATIONS;
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
    const uint64_t before = object->counter;
    if (object->semaphore) {
        value = 1;
        object->counter--;
    } else {
        value = object->counter;
        object->counter = 0;
    }
    if (before == EVENTFD_COUNTER_LIMIT) object->write_edges++;
    pthread_cond_signal(&object->writable);
    pthread_mutex_unlock(&object->lock);
    android_virtual_note_change();
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
    if (object->counter == 0 && value != 0) object->read_edges++;
    object->counter += value;
    pthread_cond_signal(&object->readable);
    pthread_mutex_unlock(&object->lock);
    android_virtual_note_change();
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
    android_virtual_note_change();
    return 0;
}
