#ifndef CORDIAL_ANDROID_MEDIA_FORMAT_COMPAT_H
#define CORDIAL_ANDROID_MEDIA_FORMAT_COMPAT_H

#include <stdbool.h>
#include <stdint.h>

// A bounded native implementation of the Android NDK AMediaFormat object.
// AOSP's libmediandk versioned exports are resolved only for their owning
// provider. No decoder/encoder implementation is implied by this adapter.
bool android_media_format_compat_resolve(const char *name, const char *version,
                                         const char *library, uint64_t *address);

#endif
