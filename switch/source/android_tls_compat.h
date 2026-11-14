#ifndef CORDIAL_ANDROID_TLS_COMPAT_H
#define CORDIAL_ANDROID_TLS_COMPAT_H

#include <stdbool.h>
#include <stdint.h>

// Android's 32-bit pthread_key_t values and thread-specific destructor
// semantics are translated without exposing libnx's limited native key pool.
bool android_tls_compat_resolve(const char *name, const char *version,
                                const char *library, uint64_t *address);

#endif
