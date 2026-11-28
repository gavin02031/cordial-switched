#ifndef CORDIAL_SWITCH_ANDROID_DL_COMPAT_H
#define CORDIAL_SWITCH_ANDROID_DL_COMPAT_H

#include <stdbool.h>
#include <stdint.h>

// A partial, truthful Bionic libdl bridge: only known native compatibility
// modules have handles. This does NOT load arbitrary Android ELF dependencies.
bool android_dl_compat_resolve(const char *name, const char *version,
                               const char *library, uint64_t *address);

#endif
