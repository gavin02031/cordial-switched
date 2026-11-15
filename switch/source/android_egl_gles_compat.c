#include "android_egl_gles_compat.h"
#include "android_native_window_compat.h"

#include <stdint.h>
#include <string.h>
#include <stdatomic.h>

#ifdef __SWITCH__
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#endif

// Keep the imported symbol catalogue explicit. Arbitrary symbols from an
// unrelated host GLES library must never satisfy a guest Android import.
// eglCreateWindowSurface and eglGetError use checked wrappers, since a guest
// ANativeWindow is not the Horizon NWindow that Mesa expects.
#define EGL_CORE_SYMBOLS(X) \
    X(eglChooseConfig) \
    X(eglCreateContext) \
    X(eglCreatePbufferSurface) \
    X(eglDestroyContext) \
    X(eglDestroySurface) \
    X(eglGetConfigAttrib) \
    X(eglGetCurrentContext) \
    X(eglGetDisplay) \
    X(eglInitialize) \
    X(eglMakeCurrent) \
    X(eglQuerySurface) \
    X(eglSwapBuffers) \
    X(eglSwapInterval) \
    X(eglTerminate)

#define GLES2_CORE_SYMBOLS(X) \
    X(glActiveTexture) \
    X(glAttachShader) \
    X(glBindAttribLocation) \
    X(glBindBuffer) \
    X(glBindFramebuffer) \
    X(glBindRenderbuffer) \
    X(glBindTexture) \
    X(glBlendFunc) \
    X(glBlendFuncSeparate) \
    X(glBufferData) \
    X(glBufferSubData) \
    X(glCheckFramebufferStatus) \
    X(glClear) \
    X(glClearColor) \
    X(glClearDepthf) \
    X(glClearStencil) \
    X(glColorMask) \
    X(glCompileShader) \
    X(glCompressedTexImage2D) \
    X(glCompressedTexSubImage2D) \
    X(glCopyTexSubImage2D) \
    X(glCreateProgram) \
    X(glCreateShader) \
    X(glCullFace) \
    X(glDeleteBuffers) \
    X(glDeleteFramebuffers) \
    X(glDeleteProgram) \
    X(glDeleteRenderbuffers) \
    X(glDeleteShader) \
    X(glDeleteTextures) \
    X(glDepthFunc) \
    X(glDepthMask) \
    X(glDisable) \
    X(glDisableVertexAttribArray) \
    X(glDrawArrays) \
    X(glDrawElements) \
    X(glEnable) \
    X(glEnableVertexAttribArray) \
    X(glFramebufferRenderbuffer) \
    X(glFramebufferTexture2D) \
    X(glGenBuffers) \
    X(glGenerateMipmap) \
    X(glGenFramebuffers) \
    X(glGenRenderbuffers) \
    X(glGenTextures) \
    X(glGetActiveUniform) \
    X(glGetError) \
    X(glGetIntegerv) \
    X(glGetProgramInfoLog) \
    X(glGetProgramiv) \
    X(glGetShaderInfoLog) \
    X(glGetShaderiv) \
    X(glGetString) \
    X(glGetUniformLocation) \
    X(glLinkProgram) \
    X(glPixelStorei) \
    X(glPolygonOffset) \
    X(glReadPixels) \
    X(glReleaseShaderCompiler) \
    X(glRenderbufferStorage) \
    X(glScissor) \
    X(glShaderSource) \
    X(glStencilFunc) \
    X(glStencilMask) \
    X(glStencilOp) \
    X(glTexImage2D) \
    X(glTexParameterf) \
    X(glTexParameterfv) \
    X(glTexParameteri) \
    X(glTexSubImage2D) \
    X(glUniform1i) \
    X(glUseProgram) \
    X(glVertexAttribPointer) \
    X(glViewport)

bool android_egl_gles_compat_known(const char *name)
{
    if (!name) return false;
    if (strcmp(name, "eglGetProcAddress") == 0 ||
        strcmp(name, "eglCreateWindowSurface") == 0 ||
        strcmp(name, "eglGetError") == 0) return true;
#define KNOWN(symbol) if (strcmp(name, #symbol) == 0) return true;
    EGL_CORE_SYMBOLS(KNOWN)
    GLES2_CORE_SYMBOLS(KNOWN)
#undef KNOWN
    return false;
}

unsigned android_egl_gles_compat_known_count(void)
{
#define COUNT(symbol) + 1u
    return 3u EGL_CORE_SYMBOLS(COUNT) GLES2_CORE_SYMBOLS(COUNT);
#undef COUNT
}

bool android_egl_gles_compat_has_backend(void)
{
#ifdef __SWITCH__
    return true;
#else
    return false;
#endif
}

#ifdef __SWITCH__
static _Thread_local EGLint pending_guest_error = EGL_SUCCESS;
static atomic_uint_fast64_t create_surface_attempts, create_surface_succeeded;
static atomic_uint_fast64_t make_current_attempts, make_current_succeeded;
static atomic_uint_fast64_t clear_calls, swap_attempts, swap_succeeded;

static EGLBoolean tracked_make_current(EGLDisplay display, EGLSurface draw,
                                       EGLSurface read, EGLContext context)
{
    atomic_fetch_add_explicit(&make_current_attempts, 1, memory_order_relaxed);
    const EGLBoolean succeeded = eglMakeCurrent(display, draw, read, context);
    if (succeeded == EGL_TRUE)
        atomic_fetch_add_explicit(&make_current_succeeded, 1, memory_order_relaxed);
    return succeeded;
}

static void tracked_gl_clear(GLbitfield mask)
{
    atomic_fetch_add_explicit(&clear_calls, 1, memory_order_relaxed);
    glClear(mask);
}

static EGLBoolean tracked_swap_buffers(EGLDisplay display, EGLSurface surface)
{
    atomic_fetch_add_explicit(&swap_attempts, 1, memory_order_relaxed);
    const EGLBoolean succeeded = eglSwapBuffers(display, surface);
    if (succeeded == EGL_TRUE)
        atomic_fetch_add_explicit(&swap_succeeded, 1, memory_order_relaxed);
    return succeeded;
}

static EGLSurface checked_create_window_surface(EGLDisplay display,
                                                EGLConfig config,
                                                EGLNativeWindowType guest,
                                                const EGLint *attributes)
{
    atomic_fetch_add_explicit(&create_surface_attempts, 1, memory_order_relaxed);
    void *horizon_window = NULL;
    if (!android_native_window_compat_horizon_handle(
            (const void *)(uintptr_t)guest, &horizon_window)) {
        // Calling native Mesa with the guest reference struct could crash
        // before EGL reports an error; refuse it before crossing the ABI.
        pending_guest_error = EGL_BAD_NATIVE_WINDOW;
        return EGL_NO_SURFACE;
    }
    const EGLSurface surface = eglCreateWindowSurface(display, config,
                                  (EGLNativeWindowType)horizon_window, attributes);
    if (surface != EGL_NO_SURFACE)
        atomic_fetch_add_explicit(&create_surface_succeeded, 1,
                                  memory_order_relaxed);
    return surface;
}

static EGLint checked_get_error(void)
{
    const EGLint pending = pending_guest_error;
    if (pending != EGL_SUCCESS) {
        pending_guest_error = EGL_SUCCESS;
        return pending;
    }
    return eglGetError();
}

static __eglMustCastToProperFunctionPointerType native_get_proc_address(const char *name)
{
    // Delegate only to registered symbols with identical ABI, including the
    // checked window-surface wrapper; never disclose raw Mesa entry points.
    uint64_t address = 0;
    if (!android_egl_gles_compat_resolve(name, NULL, NULL, &address))
        return NULL;
    return (__eglMustCastToProperFunctionPointerType)(uintptr_t)address;
}
#endif

void android_egl_gles_compat_read_guest_stats(AndroidGuestGraphicsStats *stats)
{
    if (!stats) return;
    memset(stats, 0, sizeof(*stats));
#ifdef __SWITCH__
    stats->create_surface_attempts =
        atomic_load_explicit(&create_surface_attempts, memory_order_relaxed);
    stats->create_surface_succeeded =
        atomic_load_explicit(&create_surface_succeeded, memory_order_relaxed);
    stats->make_current_attempts =
        atomic_load_explicit(&make_current_attempts, memory_order_relaxed);
    stats->make_current_succeeded =
        atomic_load_explicit(&make_current_succeeded, memory_order_relaxed);
    stats->clear_calls = atomic_load_explicit(&clear_calls, memory_order_relaxed);
    stats->swap_attempts = atomic_load_explicit(&swap_attempts, memory_order_relaxed);
    stats->swap_succeeded = atomic_load_explicit(&swap_succeeded, memory_order_relaxed);
#endif
}

bool android_egl_gles_compat_resolve(const char *name, const char *version,
                                     const char *library, uint64_t *address)
{
    if (!name || !address || version || library) return false;
    *address = 0;
#ifdef __SWITCH__
    if (strcmp(name, "eglGetProcAddress") == 0) {
        *address = (uint64_t)(uintptr_t)&native_get_proc_address;
        return true;
    }
    if (strcmp(name, "eglCreateWindowSurface") == 0) {
        *address = (uint64_t)(uintptr_t)&checked_create_window_surface;
        return true;
    }
    if (strcmp(name, "eglGetError") == 0) {
        *address = (uint64_t)(uintptr_t)&checked_get_error;
        return true;
    }
    if (strcmp(name, "eglMakeCurrent") == 0) {
        *address = (uint64_t)(uintptr_t)&tracked_make_current;
        return true;
    }
    if (strcmp(name, "eglSwapBuffers") == 0) {
        *address = (uint64_t)(uintptr_t)&tracked_swap_buffers;
        return true;
    }
    if (strcmp(name, "glClear") == 0) {
        *address = (uint64_t)(uintptr_t)&tracked_gl_clear;
        return true;
    }
#define MAP(symbol) \
    if (strcmp(name, #symbol) == 0) { \
        *address = (uint64_t)(uintptr_t)&symbol; \
        return true; \
    }
    EGL_CORE_SYMBOLS(MAP)
    GLES2_CORE_SYMBOLS(MAP)
#undef MAP
#endif
    return false;
}
