#ifndef CORDIAL_SWITCH_ANDROID_LOOPER_COMPAT_H
#define CORDIAL_SWITCH_ANDROID_LOOPER_COMPAT_H

#include <stdbool.h>
#include <stdint.h>

// Real file-descriptor polling bridge for Android NDK's ALooper API. It
// depends on the native Horizon fd/poll backend and never reports a ready
// event unless poll actually observed one. Android process/JNI setup remains
// separate from this adapter.
bool android_looper_compat_resolve(const char *name, const char *version,
                                   const char *library, uint64_t *address);

#endif
