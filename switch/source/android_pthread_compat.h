#ifndef CORDIAL_ANDROID_PTHREAD_COMPAT_H
#define CORDIAL_ANDROID_PTHREAD_COMPAT_H

#include <stdbool.h>
#include <stdint.h>

// Resolve only the Bionic pthread calls for which the Android object's
// representation is translated. Never expose libnx pthread objects directly.
bool android_pthread_compat_resolve(const char *name, const char *version,
                                   const char *library, uint64_t *address);

#endif
