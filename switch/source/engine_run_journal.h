#ifndef MORROW_ENGINE_RUN_JOURNAL_H
#define MORROW_ENGINE_RUN_JOURNAL_H

#include <stdbool.h>
#include <stddef.h>

// A sync-to-disk marker is written BEFORE entering guest code. A fatal signal
// cannot be caught reliably inside a Switch NRO; on the next app launch a
// surviving marker becomes a numbered crash report. It means an *unclean
// exit*, not proof of the root cause or an Android exception stack trace.
bool engine_run_journal_begin(const char *directory, const char *phase,
                              const char *link_report);
bool engine_run_journal_finish(const char *directory);
bool engine_run_journal_recover(const char *directory, char *saved_path,
                                size_t saved_path_capacity);

#endif
