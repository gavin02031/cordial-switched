#ifndef CORDIAL_ANDROID_LIBC_COMPAT_H
#define CORDIAL_ANDROID_LIBC_COMPAT_H

#include <stdbool.h>
#include <stdint.h>

bool android_libc_compat_resolve(const char *name, const char *version,
                                 const char *library, uint64_t *address);

#endif
