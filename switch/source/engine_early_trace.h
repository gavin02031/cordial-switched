#ifndef ENGINE_EARLY_TRACE_H
#define ENGINE_EARLY_TRACE_H

#include <stdbool.h>
#include <stdint.h>

// Append one independently durable breadcrumb to the existing constructor
// watchdog file. A guest fault can bypass the final numbered report.
bool engine_early_trace_append(const char *directory, const char *event,
                               uint64_t a, uint64_t b, uint64_t c);

#endif
