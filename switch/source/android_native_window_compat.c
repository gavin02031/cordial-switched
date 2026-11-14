#include "android_native_window_compat.h"

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum { MAX_NATIVE_WINDOWS = 8 };

typedef struct {
    void *java_surface;
    void *horizon_window;
    int32_t width, height;
    unsigned references;
    bool bound;
} NativeWindow;

static NativeWindow *windows[MAX_NATIVE_WINDOWS];
static pthread_mutex_t windows_lock = PTHREAD_MUTEX_INITIALIZER;

static NativeWindow *find_surface(void *surface)
{
    for (unsigned i = 0; i < MAX_NATIVE_WINDOWS; i++)
        if (windows[i] && windows[i]->bound &&
            windows[i]->java_surface == surface)
            return windows[i];
    return NULL;
}

static NativeWindow *find_window(const void *candidate)
{
    for (unsigned i = 0; i < MAX_NATIVE_WINDOWS; i++)
        if (windows[i] == candidate)
            return windows[i];
    return NULL;
}

static void drop_reference(NativeWindow *window)
{
    if (!window || !window->references) return;
    if (--window->references) return;
    for (unsigned i = 0; i < MAX_NATIVE_WINDOWS; i++) {
        if (windows[i] == window) {
            windows[i] = NULL;
            break;
        }
    }
    free(window);
}

static bool bind_surface(void *surface, void *horizon_window,
                         int32_t width, int32_t height)
{
    if (!surface || width <= 0 || height <= 0) return false;
    pthread_mutex_lock(&windows_lock);
    NativeWindow *existing = find_surface(surface);
    if (existing) {
        // A live guest window must never silently switch native displays.
        if (existing->horizon_window != horizon_window) {
            pthread_mutex_unlock(&windows_lock);
            return false;
        }
        existing->width = width;
        existing->height = height;
        pthread_mutex_unlock(&windows_lock);
        return true;
    }
    for (unsigned i = 0; i < MAX_NATIVE_WINDOWS; i++) {
        if (windows[i]) continue;
        NativeWindow *window = calloc(1, sizeof(*window));
        if (!window) break;
        *window = (NativeWindow){surface, horizon_window, width, height, 1, true};
        windows[i] = window;
        pthread_mutex_unlock(&windows_lock);
        return true;
    }
    pthread_mutex_unlock(&windows_lock);
    return false;
}

bool android_native_window_compat_bind_surface(void *surface,
                                                int32_t width, int32_t height)
{
    return bind_surface(surface, NULL, width, height);
}

bool android_native_window_compat_bind_horizon_surface(void *surface,
                                                        void *horizon_window,
                                                        int32_t width,
                                                        int32_t height)
{
    if (!horizon_window) return false;
    return bind_surface(surface, horizon_window, width, height);
}

bool android_native_window_compat_horizon_handle(const void *candidate,
                                                  void **horizon_window)
{
    if (!horizon_window) return false;
    *horizon_window = NULL;
    pthread_mutex_lock(&windows_lock);
    NativeWindow *window = find_window(candidate);
    if (window && window->bound && window->horizon_window)
        *horizon_window = window->horizon_window;
    pthread_mutex_unlock(&windows_lock);
    return *horizon_window != NULL;
}

void android_native_window_compat_unbind_surface(void *surface)
{
    if (!surface) return;
    pthread_mutex_lock(&windows_lock);
    NativeWindow *window = find_surface(surface);
    if (window) {
        window->bound = false;
        window->java_surface = NULL;
        drop_reference(window);
    }
    pthread_mutex_unlock(&windows_lock);
}

static NativeWindow *window_from_surface(void *env, void *surface)
{
    if (!env || !surface) return NULL;
    pthread_mutex_lock(&windows_lock);
    NativeWindow *window = find_surface(surface);
    if (window && window->references < UINT_MAX) window->references++;
    else window = NULL;
    pthread_mutex_unlock(&windows_lock);
    return window;
}

static void window_acquire(NativeWindow *candidate)
{
    pthread_mutex_lock(&windows_lock);
    NativeWindow *window = find_window(candidate);
    if (window && window->references < UINT_MAX) window->references++;
    pthread_mutex_unlock(&windows_lock);
}

static void window_release(NativeWindow *candidate)
{
    pthread_mutex_lock(&windows_lock);
    drop_reference(find_window(candidate));
    pthread_mutex_unlock(&windows_lock);
}

static int32_t window_width(NativeWindow *candidate)
{
    pthread_mutex_lock(&windows_lock);
    NativeWindow *window = find_window(candidate);
    const int32_t width = window ? window->width : -EINVAL;
    pthread_mutex_unlock(&windows_lock);
    return width;
}

static int32_t window_height(NativeWindow *candidate)
{
    pthread_mutex_lock(&windows_lock);
    NativeWindow *window = find_window(candidate);
    const int32_t height = window ? window->height : -EINVAL;
    pthread_mutex_unlock(&windows_lock);
    return height;
}

bool android_native_window_compat_resolve(const char *name, const char *version,
                                          const char *library, uint64_t *address)
{
    if (!name || !address || version || library) return false;
    *address = 0;
#define WINDOW_MAP(symbol, implementation) \
    if (strcmp(name, #symbol) == 0) \
        *address = (uint64_t)(uintptr_t)&implementation; else
    WINDOW_MAP(ANativeWindow_fromSurface, window_from_surface)
    WINDOW_MAP(ANativeWindow_acquire, window_acquire)
    WINDOW_MAP(ANativeWindow_release, window_release)
    WINDOW_MAP(ANativeWindow_getWidth, window_width)
    WINDOW_MAP(ANativeWindow_getHeight, window_height)
    (void)0;
#undef WINDOW_MAP
    return *address != 0;
}
