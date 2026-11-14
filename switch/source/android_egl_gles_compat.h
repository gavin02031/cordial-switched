#ifndef CORDIAL_SWITCH_ANDROID_EGL_GLES_COMPAT_H
#define CORDIAL_SWITCH_ANDROID_EGL_GLES_COMPAT_H

#include <stdbool.h>
#include <stdint.h>

// Identifies the core ES2 / EGL symbols that the native Mesa port can answer.
// The Android Java Surface -> Horizon NWindow bridge is NOT ready, so the
// Android eglCreateWindowSurface entry point is deliberately not exported.
bool android_egl_gles_compat_known(const char *name);
unsigned android_egl_gles_compat_known_count(void);
bool android_egl_gles_compat_has_backend(void);
bool android_egl_gles_compat_resolve(const char *name, const char *version,
                                     const char *library, uint64_t *address);

#endif
