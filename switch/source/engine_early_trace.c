#define _POSIX_C_SOURCE 200809L
#include "engine_early_trace.h"

#include <errno.h>
#include <stdatomic.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifndef MORROW_BUILD_VERSION
#define MORROW_BUILD_VERSION "development"
#endif

bool engine_early_trace_append(const char *directory, const char *event,
                               uint64_t a, uint64_t b, uint64_t c)
{
    if (!directory || !*directory || !event || !*event ||
        strlen(event) > 48) return false;
    char path[192];
    const int pn = snprintf(path, sizeof(path), "%s/constructor-watchdog.txt",
                            directory);
    if (pn <= 0 || (size_t)pn >= sizeof(path)) return false;
    char line[256];
    const int n = snprintf(line, sizeof(line),
                           "%ld build=%.48s trace_event=%s a=0x%llx b=0x%llx c=0x%llx\n",
                           (long)time(NULL), MORROW_BUILD_VERSION, event,
                           (unsigned long long)a, (unsigned long long)b,
                           (unsigned long long)c);
    if (n <= 0 || (size_t)n >= sizeof(line)) return false;
    const int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (fd < 0) return false;
    bool ok = true;
    size_t done = 0;
    while (done < (size_t)n) {
        const ssize_t count = write(fd, line + done, (size_t)n - done);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) { ok = false; break; }
        done += (size_t)count;
    }
    if (ok) ok = fsync(fd) == 0;
    if (close(fd) != 0) ok = false;
    return ok;
}

// A bounded try-lock protects the queue without heap allocation or SD I/O.
// Contended/full writes are dropped instead of re-entering allocator locks.
enum { EARLY_QUEUE_LIMIT = 32, EARLY_EVENT_LIMIT = 48 };
typedef struct {
    char event[EARLY_EVENT_LIMIT + 1];
    uint64_t a, b, c;
} EarlyQueuedEvent;
static EarlyQueuedEvent early_queue[EARLY_QUEUE_LIMIT];
static unsigned early_count;
static atomic_flag early_guard = ATOMIC_FLAG_INIT;

bool engine_early_trace_stage(const char *event,
                              uint64_t a, uint64_t b, uint64_t c)
{
    if (!event || !*event) return false;
    const size_t length = strlen(event);
    if (length > EARLY_EVENT_LIMIT) return false;
    if (atomic_flag_test_and_set_explicit(&early_guard, memory_order_acquire))
        return false;
    bool queued = early_count < EARLY_QUEUE_LIMIT;
    if (queued) {
        EarlyQueuedEvent *item = &early_queue[early_count++];
        memcpy(item->event, event, length + 1);
        item->a = a;
        item->b = b;
        item->c = c;
    }
    atomic_flag_clear_explicit(&early_guard, memory_order_release);
    return queued;
}

size_t engine_early_trace_flush(const char *directory)
{
    if (!directory || !*directory) return 0;
    char path[192];
    const int pn = snprintf(path, sizeof(path), "%s/constructor-watchdog.txt",
                            directory);
    if (pn <= 0 || (size_t)pn >= sizeof(path)) return 0;

    // Open before draining so the queue survives a temporarily inaccessible
    // SD card. Never hold the producer try-lock while doing filesystem I/O.
    const int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (fd < 0) return 0;
    if (atomic_flag_test_and_set_explicit(&early_guard, memory_order_acquire)) {
        close(fd);
        return 0;
    }
    EarlyQueuedEvent snapshot[EARLY_QUEUE_LIMIT];
    const unsigned count = early_count;
    if (count) memcpy(snapshot, early_queue, count * sizeof(snapshot[0]));
    early_count = 0;
    atomic_flag_clear_explicit(&early_guard, memory_order_release);

    // One open/fsync/close per batch instead of up to 32 full SD syncs.
    // The independent watchdog can return to register sampling promptly.
    const long stamp = (long)time(NULL);
    size_t persisted = 0;
    for (unsigned i = 0; i < count; ++i) {
        char line[256];
        const int n = snprintf(line, sizeof(line),
            "%ld build=%.48s trace_event=%s a=0x%llx b=0x%llx c=0x%llx\n",
            stamp, MORROW_BUILD_VERSION, snapshot[i].event,
            (unsigned long long)snapshot[i].a,
            (unsigned long long)snapshot[i].b,
            (unsigned long long)snapshot[i].c);
        if (n <= 0 || (size_t)n >= sizeof(line)) break;
        size_t done = 0;
        while (done < (size_t)n) {
            const ssize_t bytes = write(fd, line + done, (size_t)n - done);
            if (bytes < 0 && errno == EINTR) continue;
            if (bytes <= 0) break;
            done += (size_t)bytes;
        }
        if (done != (size_t)n) break;
        ++persisted;
    }
    if (persisted) fsync(fd);
    close(fd);
    return persisted;
}
