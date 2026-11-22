#ifndef CORDIAL_ANDROID_LIBC_COMPAT_H
#define CORDIAL_ANDROID_LIBC_COMPAT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

bool android_libc_compat_resolve(const char *name, const char *version,
                                 const char *library, uint64_t *address);

// Raw getrandom(2) over the same real entropy source getentropy uses.
// Only GRND_NONBLOCK and GRND_RANDOM are accepted; unknown flag bits get
// EINVAL. Returns the byte count, never a short read, or -1 with errno.
long android_getrandom(void *buffer, size_t count, unsigned flags);

#endif
