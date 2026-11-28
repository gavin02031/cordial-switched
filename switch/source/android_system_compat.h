#ifndef CORDIAL_SWITCH_ANDROID_SYSTEM_COMPAT_H
#define CORDIAL_SWITCH_ANDROID_SYSTEM_COMPAT_H

#include <stdbool.h>
#include <stdint.h>

bool android_system_compat_resolve(const char *name, const char *version,
                                   const char *library, uint64_t *address);

// The most recent message captured through android_set_abort_message, or an
// empty string. Diagnostics only; the guest retains no reference to it.
const char *android_system_compat_abort_message(void);

#endif
