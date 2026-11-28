#ifndef MORROW_ANDROID_WCHAR_COMPAT_H
#define MORROW_ANDROID_WCHAR_COMPAT_H

#include <stdbool.h>
#include <stdint.h>

// AOSP Bionic arm64 mbstate_t has four UTF-8 sequence bytes and four
// reserved bytes. Do not pass this opaque guest storage to native newlib.
bool android_wchar_compat_resolve(const char *name, const char *version,
                                  const char *library, uint64_t *address);

#endif
