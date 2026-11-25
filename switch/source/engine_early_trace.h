#ifndef ENGINE_EARLY_TRACE_H
#define ENGINE_EARLY_TRACE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Append one independently durable breadcrumb to the existing constructor
// watchdog file. A guest fault can bypass the final numbered report.
bool engine_early_trace_append(const char *directory, const char *event,
                               uint64_t a, uint64_t b, uint64_t c);

// Guest-provider hooks must never perform blocking SD I/O or fsync. Events
// are staged in a fixed-size, allocation-free bounded queue, then drained
// by the independent constructor watchdog or after a constructor returns.
bool engine_early_trace_stage(const char *event,
                              uint64_t a, uint64_t b, uint64_t c);
size_t engine_early_trace_flush(const char *directory);

#endif
