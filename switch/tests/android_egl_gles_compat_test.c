#include "android_egl_gles_compat.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

int main(void)
{
    // The symbol catalogue can be audited on a host, without pretending
    // Linux's GL implementation is the Nintendo Switch Mesa backend.
    assert(android_egl_gles_compat_known_count() == 90);
    assert(android_egl_gles_compat_known("eglInitialize"));
    assert(android_egl_gles_compat_known("eglCreatePbufferSurface"));
    assert(android_egl_gles_compat_known("eglGetProcAddress"));
    assert(android_egl_gles_compat_known("glCreateShader"));
    assert(android_egl_gles_compat_known("glUniform1i"));
    assert(android_egl_gles_compat_known("glViewport"));
    assert(!android_egl_gles_compat_known("eglCreateWindowSurface"));
    assert(!android_egl_gles_compat_known("glRandomUnimplementedExtension"));
    assert(!android_egl_gles_compat_known("ANativeWindow_fromSurface"));
    assert(!android_egl_gles_compat_known(NULL));

    uint64_t address = 0xabad1dea;
    assert(!android_egl_gles_compat_has_backend());
    assert(!android_egl_gles_compat_resolve("eglInitialize", NULL, NULL, &address));
    assert(address == 0); // A host build must not claim a Horizon provider.
    assert(!android_egl_gles_compat_resolve("glCreateShader", NULL, NULL, &address));
    assert(address == 0);
    assert(!android_egl_gles_compat_resolve("glCreateShader", "LIBC", "libGLESv2.so", &address));
    assert(!android_egl_gles_compat_resolve("eglInitialize", NULL, "libEGL.so", &address));
    assert(!android_egl_gles_compat_resolve(NULL, NULL, NULL, &address));
    assert(!android_egl_gles_compat_resolve("glCreateShader", NULL, NULL, NULL));
    puts("android_egl_gles_compat_test: 90 names checked, no fake host backend");
    return 0;
}
