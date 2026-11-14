#include "android_native_window_compat.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef void *(*FromSurface)(void *, void *);
typedef void (*Ref)(void *);
typedef int32_t (*Dimension)(void *);

static void resolved(const char *name, void *out)
{
    uint64_t address = 0;
    assert(android_native_window_compat_resolve(name, NULL, NULL, &address));
    assert(address);
    memcpy(out, &address, sizeof(address));
}

int main(void)
{
    FromSurface from_surface;
    Ref acquire, release;
    Dimension width, height;
    resolved("ANativeWindow_fromSurface", &from_surface);
    resolved("ANativeWindow_acquire", &acquire);
    resolved("ANativeWindow_release", &release);
    resolved("ANativeWindow_getWidth", &width);
    resolved("ANativeWindow_getHeight", &height);

    uint64_t unresolved = 0;
    assert(!android_native_window_compat_resolve("ANativeWindow_lock", NULL, NULL,
                                                 &unresolved));
    assert(!android_native_window_compat_resolve("ANativeWindow_acquire", "LIBC",
                                                 "libc.so", &unresolved));
    int surface_1 = 1, surface_2 = 2, env = 3;
    assert(from_surface(&env, &surface_1) == NULL);
    assert(!android_native_window_compat_bind_surface(NULL, 1280, 720));
    assert(!android_native_window_compat_bind_surface(&surface_1, 0, 720));
    assert(android_native_window_compat_bind_surface(&surface_1, 1280, 720));
    assert(from_surface(NULL, &surface_1) == NULL);
    void *window = from_surface(&env, &surface_1);
    assert(window && width(window) == 1280 && height(window) == 720);
    assert(android_native_window_compat_bind_surface(&surface_1, 960, 540));
    assert(width(window) == 960 && height(window) == 540);
    acquire(window);
    release(window);
    assert(width(window) == 960);
    android_native_window_compat_unbind_surface(&surface_1);
    assert(from_surface(&env, &surface_1) == NULL);
    assert(width(window) == 960); // Borrowed reference remains valid.
    assert(android_native_window_compat_bind_surface(&surface_2, 1280, 720));
    void *other = from_surface(&env, &surface_2);
    assert(other && other != window && height(other) == 720);
    release(window);
    assert(width(window) < 0); // Released native reference is no longer valid.
    release(other);
    android_native_window_compat_unbind_surface(&surface_2);
    assert(from_surface(&env, &surface_2) == NULL);
    puts("android_native_window_compat_test: all assertions passed");
    return 0;
}
