#ifndef MORROW_UI_LOGS_H
#define MORROW_UI_LOGS_H

#include <stdbool.h>
#include <stddef.h>

// Bounded, read-only SD-card diagnostics. This module also builds on a host PC.
#define MORROW_LOG_MAX_FILES 32
#define MORROW_LOG_MAX_LINES 48
#define MORROW_LOG_LINE_LENGTH 112

typedef struct {
    char name[80];
    unsigned long bytes;
    long modified;
} MorrowLogFile;

typedef struct {
    MorrowLogFile files[MORROW_LOG_MAX_FILES];
    unsigned count;
    unsigned selected;
    char lines[MORROW_LOG_MAX_LINES][MORROW_LOG_LINE_LENGTH];
    unsigned line_count;
    bool read_error;
} MorrowLogs;

// Restrict reads to diagnostic .txt files, never other SD-card content.
bool morrow_log_allowed(const char *name);
void morrow_logs_scan(MorrowLogs *logs, const char *directory);
bool morrow_logs_open(MorrowLogs *logs, const char *directory, unsigned index);

#endif
