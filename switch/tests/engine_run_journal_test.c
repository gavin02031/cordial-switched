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

    // The exclusive guest-message loop may enter the same callback 120
    // times. Each successful return must free its durable marker so the
    // next call can be journaled without overwriting any prior crash.
    for (unsigned i = 1; i <= 120; i++) {
        char phase[80];
        snprintf(phase, sizeof(phase), "app_bridge_pump_tick_%03u", i);
        assert(engine_run_journal_begin(folder, phase, "link-05.txt"));
        assert(engine_run_journal_finish(folder));
    }
    assert(!engine_run_journal_recover(folder, output, sizeof(output)));

    // A new run that dies inside the next callback is recovered with its
    // exact phase rather than being mistaken for a completed pump cycle.
    assert(engine_run_journal_begin(folder, "app_bridge_pump_tick_121",
                                    "link-05.txt"));
    assert(engine_run_journal_recover(folder, output, sizeof(output)));
    assert(strstr(output, "crash-03.txt"));
    assert_contains(output, "phase=app_bridge_pump_tick_121");
    assert_contains(output, "link_report=link-05.txt");

    // An actual guest return only completes the journal after the report
    // containing that return has reached durable storage.
    FILE *report = tmpfile();
    assert(report);
    assert(engine_run_journal_begin(folder, "jni_onload_returned", "link-06.txt"));
    assert(fputs("engine_jni_onload_return=0x00010006\n", report) >= 0);
    assert(engine_run_journal_finish_report(folder, report));
    assert(!engine_run_journal_recover(folder, output, sizeof(output)));
    assert(fclose(report) == 0);

    // A pipe accepts the report text but cannot be fsynced. Losing the
    // marker here would hide the interrupted persistence step entirely.
    int ends[2];
    assert(pipe(ends) == 0);
    FILE *unsyncable = fdopen(ends[1], "w");
    assert(unsyncable);
    assert(engine_run_journal_begin(folder, "app_bridge_5_returned", "link-07.txt"));
    assert(fputs("app_bridge_step_5_returned=yes\n", unsyncable) >= 0);
    assert(!engine_run_journal_finish_report(folder, unsyncable));
    assert(!engine_run_journal_begin(folder, "must_not_replace", "link-08.txt"));
    assert(fclose(unsyncable) == 0);
    assert(close(ends[0]) == 0);
    assert(engine_run_journal_recover(folder, output, sizeof(output)));
    assert(strstr(output, "crash-04.txt"));
    assert_contains(output, "phase=app_bridge_5_returned");
    assert_contains(output, "link_report=link-07.txt");

    char first[192], second[192], third[192], fourth[192];
    snprintf(first, sizeof(first), "%s/crash-01.txt", folder);
    snprintf(second, sizeof(second), "%s/crash-02.txt", folder);
    snprintf(third, sizeof(third), "%s/crash-03.txt", folder);
    snprintf(fourth, sizeof(fourth), "%s/crash-04.txt", folder);
    assert(unlink(first) == 0);
    assert(unlink(second) == 0);
    assert(unlink(third) == 0);
    assert(unlink(fourth) == 0);
    assert(rmdir(folder) == 0);
    puts("engine crash journal passed");
    return 0;
}
