#ifndef CORDIAL_ANDROID_THREAD_LIFECYCLE_COMPAT_H
#define CORDIAL_ANDROID_THREAD_LIFECYCLE_COMPAT_H

#include <stdbool.h>
#include <stdint.h>

// A bounded bridge for Bionic's opaque pthread_t tokens. This does not
// initialise the Bionic TCB/TLS required by a live Android engine.
bool android_thread_lifecycle_compat_resolve(const char *name, const char *version,
                                             const char *library, uint64_t *address);

#endif
