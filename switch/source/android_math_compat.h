#ifndef CORDIAL_SWITCH_ANDROID_MATH_COMPAT_H
#define CORDIAL_SWITCH_ANDROID_MATH_COMPAT_H

#include <stdbool.h>
#include <stdint.h>

// Only ordinary float/double C math ABIs that match AArch64's hardware calling
// convention. The Android provider+symbol version are mandatory. No long
// double, errno-dependent or Android-specific data interfaces are exposed.
bool android_math_compat_resolve(const char *name, const char *version,
                                 const char *library, uint64_t *address);

#endif
