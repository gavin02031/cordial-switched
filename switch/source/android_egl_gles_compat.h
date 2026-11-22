#ifndef CORDIAL_SWITCH_ANDROID_EGL_GLES_COMPAT_H
#define CORDIAL_SWITCH_ANDROID_EGL_GLES_COMPAT_H

#include <stdbool.h>
#include <stdint.h>

// Identifies the core EGL / ES2 / ES3.0 / ES3.2 symbols that the native
// Mesa port can answer. The ES3 entries are verified against Mesa 20.1's
// libGLESv2 ABI list; EXT/OES/KHR variants and unexported core names stay
// uncatalogued so guest probes see an honest NULL.
// Window-surface creation is exposed only through a checked guest-window
// adapter. It refuses unregistered Java/ANativeWindow handles; the Java
// runtime must first bind a real Horizon NWindow through the window adapter.
bool android_egl_gles_compat_known(const char *name);
unsigned android_egl_gles_compat_known_count(void);
bool android_egl_gles_compat_has_backend(void);
bool android_egl_gles_compat_resolve(const char *name, const char *version,
                                     const char *library, uint64_t *address);

// Only calls through symbols supplied to the guest are counted; SDL's own
// renderer does not go through these wrappers. These are *activity counters*,
// not proof that a visible Roblox frame reached the Switch display.
typedef struct {
    uint64_t create_surface_attempts;
    uint64_t create_surface_succeeded;
    uint64_t make_current_attempts;
    uint64_t make_current_succeeded;
    uint64_t clear_calls;
    uint64_t swap_attempts;
    uint64_t swap_succeeded;
} AndroidGuestGraphicsStats;

void android_egl_gles_compat_read_guest_stats(AndroidGuestGraphicsStats *stats);

#endif
