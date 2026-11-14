#ifndef CORDIAL_SWITCH_ANDROID_NATIVE_WINDOW_COMPAT_H
#define CORDIAL_SWITCH_ANDROID_NATIVE_WINDOW_COMPAT_H

#include <stdbool.h>
#include <stdint.h>

// A JNI bridge must first register a *verified* Java Surface. An arbitrary
// jobject is not a native window and cannot be cast into a Horizon handle.
// Bind takes one owner reference; unbind releases it without invalidating
// outstanding native references from ANativeWindow_fromSurface/acquire.
bool android_native_window_compat_bind_surface(void *java_surface,
                                                int32_t width, int32_t height);
void android_native_window_compat_unbind_surface(void *java_surface);

// Only libandroid's window-reference and dimension APIs are implemented;
// no EGL surface or Java Surface object is created by this adapter.
bool android_native_window_compat_resolve(const char *name, const char *version,
                                          const char *library, uint64_t *address);

#endif
