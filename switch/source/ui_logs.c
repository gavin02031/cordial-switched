#include "ui_logs.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

bool morrow_log_allowed(const char *name)
{
    static const char *const prefixes[] = {
        "link-", "crash-", "diag-", "stage-", "engine-",
        "constructor-", "exception-", "bootstrap-", "loader-",
        "jni-", "guest-", "gles-", "morrow-", "imports-",
        "map-", "asset-", "trace-", "egl-", "runtime-"
    };
    if (!name || strlen(name) > 79)
        return false;
    const size_t len = strlen(name);
    if (len < 5 || strcmp(name + len - 4, ".txt") != 0)
        return false;
    for (size_t i = 0; i < len; ++i)
        if (!(isalnum((unsigned char)name[i]) || name[i] == '-' ||
              name[i] == '_' || name[i] == '.'))
            return false;
    for (size_t i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); i++)
        if (strncmp(name, prefixes[i], strlen(prefixes[i])) == 0)
            return true;
    return false;
}

static bool path_join(char *out, size_t cap, const char *dir, const char *name)
{
    return snprintf(out, cap, "%s/%s", dir, name) < (int)cap;
}

// Recent files first. Lexical names break ties consistently on FAT volumes.
static bool more_recent(const MorrowLogFile *a, const MorrowLogFile *b)
{
    return a->modified > b->modified ||
           (a->modified == b->modified && strcmp(a->name, b->name) > 0);
}

void morrow_logs_scan(MorrowLogs *logs, const char *directory)
{
    memset(logs, 0, sizeof(*logs));
    DIR *dir = opendir(directory);
    if (!dir) {
        logs->read_error = true;
        return;
    }
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (!morrow_log_allowed(entry->d_name))
            continue;
        char path[256];
        struct stat st;
        if (!path_join(path, sizeof(path), directory, entry->d_name) ||
            stat(path, &st) || !S_ISREG(st.st_mode))
            continue;
        MorrowLogFile next = {0};
        // morrow_log_allowed already bounds the filename to 79 bytes.
        memcpy(next.name, entry->d_name, strlen(entry->d_name) + 1);
        next.bytes = st.st_size > 0 ? (unsigned long)st.st_size : 0;
        next.modified = (long)st.st_mtime;
        unsigned place = 0;
        while (place < logs->count &&
               !more_recent(&next, &logs->files[place]))
            ++place;
        if (place >= MORROW_LOG_MAX_FILES)
            continue;
        if (logs->count < MORROW_LOG_MAX_FILES)
            ++logs->count;
        for (unsigned j = logs->count - 1; j > place; --j)
            logs->files[j] = logs->files[j - 1];
        logs->files[place] = next;
    }
    closedir(dir);
}

bool morrow_logs_open(MorrowLogs *logs, const char *directory, unsigned index)
{
    logs->line_count = 0;
    logs->read_error = false;
    if (index >= logs->count || !morrow_log_allowed(logs->files[index].name))
        return false;
    logs->selected = index;

    char path[256];
    if (!path_join(path, sizeof(path), directory, logs->files[index].name))
        return false;
    FILE *file = fopen(path, "rb");
    if (!file) {
        logs->read_error = true;
        return false;
    }

    // Read only the final 32 KiB, retaining the last 48 printable lines.
    // Huge linker traces cannot exhaust memory or block the render loop.
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        logs->read_error = true;
        return false;
    }
    const long length = ftell(file);
    if (length < 0) {
        fclose(file);
        logs->read_error = true;
        return false;
    }
    const long start = length > 32768 ? length - 32768 : 0;
    if (fseek(file, start, SEEK_SET) != 0) {
        fclose(file);
        logs->read_error = true;
        return false;
    }
    if (start) {
        int c;
        while ((c = fgetc(file)) != EOF && c != '\n') { }
    }

    char line[512];
    while (fgets(line, sizeof(line), file)) {
        // Long lines may be split across entries; all content remains bounded.
        unsigned slot;
        if (logs->line_count < MORROW_LOG_MAX_LINES)
            slot = logs->line_count++;
        else {
            memmove(logs->lines[0], logs->lines[1],
                    (MORROW_LOG_MAX_LINES - 1) * MORROW_LOG_LINE_LENGTH);
            slot = MORROW_LOG_MAX_LINES - 1;
        }
        size_t j = 0;
        for (size_t i = 0; line[i] && j < MORROW_LOG_LINE_LENGTH - 1; ++i)
            if (line[i] == '\t' || (unsigned char)line[i] >= 32)
                logs->lines[slot][j++] = line[i] == '\t' ? ' ' :
                    (unsigned char)line[i] > 126 ? '?' : line[i];
        logs->lines[slot][j] = '\0';
    }
    fclose(file);
    return true;
}
