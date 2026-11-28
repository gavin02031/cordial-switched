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
    void *translated = (void *)(uintptr_t)0xdeadbeef;
    assert(from_surface(&env, &surface_1) == NULL);
    assert(!android_native_window_compat_horizon_handle(&surface_1, &translated));
    assert(translated == NULL);
    assert(!android_native_window_compat_horizon_handle(NULL, NULL));
    assert(!android_native_window_compat_bind_surface(NULL, 1280, 720));
    assert(!android_native_window_compat_bind_surface(&surface_1, 0, 720));
    assert(android_native_window_compat_bind_surface(&surface_1, 1280, 720));
    assert(from_surface(NULL, &surface_1) == NULL);
    void *window = from_surface(&env, &surface_1);
    assert(window && width(window) == 1280 && height(window) == 720);
    assert(!android_native_window_compat_horizon_handle(window, &translated));
    assert(translated == NULL); // Dimensions alone are not a Mesa window.
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

    int surface_3 = 3, real_window = 4, different_window = 5;
    assert(!android_native_window_compat_bind_horizon_surface(
        &surface_3, NULL, 1280, 720));
    assert(android_native_window_compat_bind_horizon_surface(
        &surface_3, &real_window, 1280, 720));
    assert(!android_native_window_compat_bind_horizon_surface(
        &surface_3, &different_window, 1280, 720));
    assert(!android_native_window_compat_bind_surface(&surface_3, 1280, 720));
    assert(!android_native_window_compat_horizon_handle(&surface_3, &translated));
    assert(translated == NULL); // jobject is not a guest ANativeWindow.
    void *guest = from_surface(&env, &surface_3);
    assert(guest && android_native_window_compat_horizon_handle(guest, &translated));
    assert(translated == &real_window);
    assert(android_native_window_compat_bind_horizon_surface(
        &surface_3, &real_window, 960, 540));
    assert(width(guest) == 960 && height(guest) == 540);
    android_native_window_compat_unbind_surface(&surface_3);
    assert(!android_native_window_compat_horizon_handle(guest, &translated));
    assert(translated == NULL); // An unbound Surface cannot create a new EGL surface.
    release(guest);
    puts("android_native_window_compat_test: all assertions passed");
    return 0;
}
