#pragma once

#include <stdbool.h>
#include <stdint.h>

bool android_process_compat_resolve(const char *name, const char *version,
                                    const char *library, uint64_t *address);
