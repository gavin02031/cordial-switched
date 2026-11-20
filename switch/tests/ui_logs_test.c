#define _POSIX_C_SOURCE 200809L
#include "ui_logs.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void write_lines(const char *path, unsigned count)
{
    FILE *file = fopen(path, "w");
    assert(file);
    for (unsigned i = 0; i < count; i++)
        assert(fprintf(file, "linker event %u\n", i) > 0);
    assert(fclose(file) == 0);
}

int main(void)
{
    assert(morrow_log_allowed("link-53.txt"));
    assert(morrow_log_allowed("constructor-watchdog.txt"));
    assert(morrow_log_allowed("diag-progress.txt"));
    assert(!morrow_log_allowed("../../etc/passwd"));
    assert(!morrow_log_allowed("secret.txt"));
    assert(!morrow_log_allowed("link-53.so"));

    char directory[] = "/tmp/morrow-ui-logs-XXXXXX";
    assert(mkdtemp(directory));
    char log_path[256];
    char ignored_path[256];
    snprintf(log_path, sizeof(log_path), "%s/link-53.txt", directory);
    snprintf(ignored_path, sizeof(ignored_path), "%s/notes.txt", directory);
    write_lines(log_path, 70);
    write_lines(ignored_path, 3);

    MorrowLogs logs = {0};
    morrow_logs_scan(&logs, directory);
    assert(!logs.read_error);
    assert(logs.count == 1);
    assert(strcmp(logs.files[0].name, "link-53.txt") == 0);
    assert(morrow_logs_open(&logs, directory, 0));
    assert(logs.line_count == MORROW_LOG_MAX_LINES);
    assert(strcmp(logs.lines[0], "linker event 22") == 0);
    assert(strcmp(logs.lines[47], "linker event 69") == 0);
    assert(!morrow_logs_open(&logs, directory, 1));
    assert(!logs.line_count);

    assert(unlink(log_path) == 0);
    assert(unlink(ignored_path) == 0);
    assert(rmdir(directory) == 0);
    puts("Morrow native SD log viewer: pass");
    return 0;
}
