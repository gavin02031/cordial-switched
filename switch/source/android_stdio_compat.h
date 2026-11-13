#ifndef CORDIAL_ANDROID_STDIO_COMPAT_H
#define CORDIAL_ANDROID_STDIO_COMPAT_H

#include <stdbool.h>
#include <stdint.h>

// Resolve only Android Bionic libc symbols whose ABI is implemented here.
// The returned address for stderr/stdout/stdin is a FILE *data symbol,
// not the host's FILE object or the address of a host stream.
bool android_stdio_compat_resolve(const char *name, const char *version,
                                  const char *library, uint64_t *address);

#endif
