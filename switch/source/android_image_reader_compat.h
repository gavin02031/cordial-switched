#ifndef CORDIAL_ANDROID_IMAGE_READER_COMPAT_H
#define CORDIAL_ANDROID_IMAGE_READER_COMPAT_H

#include <stdbool.h>
#include <stdint.h>

// Android NDK ImageReader/HardwareBuffer import shim. Constructors explicitly
// report unsupported and never return fabricated media or GPU objects. The
// ABI is shared with Cordial's desktop image_reader.rs implementation.
bool android_image_reader_compat_resolve(const char *name, const char *version,
                                         const char *library, uint64_t *address);

#endif
