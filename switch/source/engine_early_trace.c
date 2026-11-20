#define _POSIX_C_SOURCE 200809L
#include "engine_early_trace.h"

#include <errno.h>
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
