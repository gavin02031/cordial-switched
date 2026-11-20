#ifndef CORDIAL_ANDROID_COVERAGE_COMPAT_H
#define CORDIAL_ANDROID_COVERAGE_COMPAT_H

#include <stdbool.h>
#include <stdint.h>

// Gcov hooks are diagnostics, not Roblox engine services. The Switch build
// has no guest gcov profile registry; an attempted dump is reported as lost.
bool android_coverage_compat_resolve(const char *name, const char *version,
                                     const char *library, uint64_t *address);

#endif
