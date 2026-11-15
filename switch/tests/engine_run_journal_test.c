#define _POSIX_C_SOURCE 200809L
#include "engine_run_journal.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void assert_contains(const char *path, const char *needle)
{
    FILE *file = fopen(path, "rb");
    assert(file);
    char data[1024] = {0};
    assert(fread(data, 1, sizeof(data) - 1, file) > 0);
    assert(strstr(data, needle));
    fclose(file);
}

int main(void)
{
    char folder[] = "/tmp/morrow-journal-test-XXXXXX";
    assert(mkdtemp(folder));
    char output[192];
    assert(!engine_run_journal_recover(folder, output, sizeof(output)));
    assert(!output[0]);
    assert(engine_run_journal_begin(folder, "jni_onload", "link-01.txt"));
    assert(!engine_run_journal_begin(folder, "duplicate", "link-02.txt"));
    assert(engine_run_journal_finish(folder));
    assert(!engine_run_journal_recover(folder, output, sizeof(output)));
    assert(engine_run_journal_begin(folder, "app_bridge_1_nativeGameGlobalInit",
                                    "link-03.txt"));
    // Simulate a process dying before it could clear the durable marker.
    assert(engine_run_journal_recover(folder, output, sizeof(output)));
    assert(strstr(output, "crash-01.txt"));
    assert_contains(output, "status=unclean_exit_possible_crash");
    assert_contains(output, "phase=app_bridge_1_nativeGameGlobalInit");
    assert_contains(output, "link_report=link-03.txt");
    assert(!engine_run_journal_recover(folder, output, sizeof(output)));
    assert(engine_run_journal_begin(folder, "app_bridge_2", "link-04.txt"));
    assert(engine_run_journal_recover(folder, output, sizeof(output)));
    assert(strstr(output, "crash-02.txt"));
    assert_contains(output, "link_report=link-04.txt");

    char first[192], second[192];
    snprintf(first, sizeof(first), "%s/crash-01.txt", folder);
    snprintf(second, sizeof(second), "%s/crash-02.txt", folder);
    assert(unlink(first) == 0);
    assert(unlink(second) == 0);
    assert(rmdir(folder) == 0);
    puts("engine crash journal passed");
    return 0;
}
