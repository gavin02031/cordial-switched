#ifndef CORDIAL_SWITCH_ANDROID_CONFIGURATION_COMPAT_H
#define CORDIAL_SWITCH_ANDROID_CONFIGURATION_COMPAT_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// The nine AConfiguration NDK symbols imported by the inspected ARM64 client.
// New configurations start unspecified. fromAssetManager only fills them
// when the JNI owner has explicitly supplied metadata for a live manager.
bool android_configuration_compat_resolve(const char *name, const char *version,
                                           const char *library, uint64_t *address);

#ifdef __cplusplus
}
#endif

#endif
