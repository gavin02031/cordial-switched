#include "android_coverage_compat.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

// GCC's gcov.h declares __gcov_dump(void) and a historical __gcov_flush(void).
// The guest's profile counters are not registered with libnx, so an actual
// .gcda file cannot be written. Do not claim one was emitted: warn on first
// invocation. This never changes gameplay state or pretends to be a decoder.
static atomic_flag warned = ATOMIC_FLAG_INIT;

static void coverage_unavailable(void)
{
    if (!atomic_flag_test_and_set(&warned))
        fputs("Morrow: guest gcov profile dump unavailable; no coverage data saved\n",
              stderr);
}

bool android_coverage_compat_resolve(const char *name, const char *version,
                                     const char *library, uint64_t *address)
{
    if (!name || !address || version || library) return false;
    *address = 0;
    if (strcmp(name, "__gcov_dump") == 0 ||
        strcmp(name, "__gcov_flush") == 0)
        *address = (uint64_t)(uintptr_t)&coverage_unavailable;
    return *address != 0;
}
