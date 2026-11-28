#ifndef CORDIAL_SWITCH_ANDROID_CXA_COMPAT_H
#define CORDIAL_SWITCH_ANDROID_CXA_COMPAT_H

#include <stdbool.h>
#include <stdint.h>

// Bionic libc's __cxa_atexit/__cxa_finalize ABI. Registration is owned by
// the Switch bridge: it must not be mixed with newlib's host C++ atexit list.
// Functions are available only with the actual LIBC/libc.so symbol version.
bool android_cxa_compat_resolve(const char *name, const char *version,
                                const char *library, uint64_t *address);

#endif
