#define _POSIX_C_SOURCE 200809L
#include "engine_run_journal.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static bool path_for(char *out, size_t size, const char *dir, const char *name)
{
    if (!out || !size || !dir || !name) return false;
    const int count = snprintf(out, size, "%s/%s", dir, name);
    return count > 0 && (size_t)count < size;
}

static bool sync_line(FILE *stream, const char *status, const char *phase,
                      const char *report)
{
    if (fprintf(stream, "status=%s\nphase=%.70s\nlink_report=%.112s\n",
                status, phase, report) < 0) return false;
    if (fflush(stream) != 0) return false;
    return fsync(fileno(stream)) == 0;
}

bool engine_run_journal_begin(const char *directory, const char *phase,
                              const char *link_report)
{
    if (!phase || !*phase || !link_report || !*link_report) return false;
    char path[192];
    if (!path_for(path, sizeof(path), directory, "engine-in-progress.txt"))
        return false;
    // Do not overwrite an unarchived crash marker from another attempt.
    const int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0) return false;
    FILE *stream = fdopen(fd, "w");
    if (!stream) { close(fd); return false; }
    const bool written = sync_line(stream, "in_progress", phase, link_report);
    const bool closed = fclose(stream) == 0;
    if (!written || !closed) {
        // Leave a failed marker in place. Recovery must not claim that the
        // guest code ran; no call is allowed on a journal write failure.
        return false;
    }
    return true;
}

bool engine_run_journal_finish(const char *directory)
{
    char path[192];
    if (!path_for(path, sizeof(path), directory, "engine-in-progress.txt"))
        return false;
    // A successful guest return clears the marker. The link report separately
    // records whether it returned successfully, failed JNI or threw C++.
    return unlink(path) == 0;
}

bool engine_run_journal_recover(const char *directory, char *saved_path,
                                size_t saved_path_capacity)
{
    if (saved_path && saved_path_capacity) saved_path[0] = '\0';
    char marker[192];
    if (!path_for(marker, sizeof(marker), directory, "engine-in-progress.txt"))
        return false;
    FILE *source = fopen(marker, "rb");
    if (!source) return false;

    char phase[80] = "unknown", report[120] = "unknown";
    char line[160];
    bool valid_status = false;
    while (fgets(line, sizeof(line), source)) {
        const size_t length = strlen(line);
        if (length && line[length - 1] == '\n') line[length - 1] = '\0';
        if (strcmp(line, "status=in_progress") == 0) valid_status = true;
        if (strncmp(line, "phase=", 6) == 0)
            snprintf(phase, sizeof(phase), "%.70s", line + 6);
        if (strncmp(line, "link_report=", 12) == 0)
            snprintf(report, sizeof(report), "%.112s", line + 12);
    }
    fclose(source);

    // Never erase an unrecognised marker, and never overwrite old reports.
    if (!valid_status) return false;
    for (unsigned i = 1; i <= 99; ++i) {
        char name[32], path[192];
        snprintf(name, sizeof(name), "crash-%02u.txt", i);
        if (!path_for(path, sizeof(path), directory, name)) return false;
        const int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
        if (fd < 0) {
            if (errno == EEXIST) continue;
            return false;
        }
        FILE *target = fdopen(fd, "w");
        if (!target) { close(fd); return false; }
        const bool written = sync_line(target, "unclean_exit_possible_crash",
                                       phase, report);
        const bool closed = fclose(target) == 0;
        if (!written || !closed) return false;
        if (unlink(marker) != 0) return false;
        if (saved_path && saved_path_capacity)
            snprintf(saved_path, saved_path_capacity, "%s", path);
        return true;
    }
    return false;
}
