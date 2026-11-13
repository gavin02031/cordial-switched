#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "android_dl_compat.h"
#include "android_log_compat.h"
#include "android_looper_compat.h"
#include "android_math_compat.h"

typedef void *(*OpenLibrary)(const char *, int);
typedef void *(*LookupSymbol)(void *, const char *);
typedef int (*CloseLibrary)(void *);
typedef char *(*GetError)(void);
typedef int (*FindAddress)(const void *, void *);

static OpenLibrary open_library;
static LookupSymbol lookup_symbol;
static CloseLibrary close_library;
static GetError get_error;
static FindAddress find_address;

static void resolve(const char *name, void *target)
{
    uint64_t address = 0;
    assert(android_dl_compat_resolve(name, "LIBC", "libdl.so", &address));
    assert(address);
    memcpy(target, &address, sizeof(address));
}

static void *separate_thread(void *unused)
{
    (void)unused;
    assert(get_error() == NULL);
    assert(open_library("libnotthere.so", 0) == NULL);
    assert(strstr(get_error(), "not available"));
    assert(get_error() == NULL);
    return NULL;
}

int main(void)
{
    resolve("dlopen", &open_library);
    resolve("dlsym", &lookup_symbol);
    resolve("dlclose", &close_library);
    resolve("dlerror", &get_error);
    resolve("dladdr", &find_address);
    uint64_t invalid = 0;
    assert(!android_dl_compat_resolve("dlopen", "WRONG", "libdl.so", &invalid));
    assert(!android_dl_compat_resolve("dlopen", "LIBC", "libc.so", &invalid));
    assert(!android_dl_compat_resolve("missing", "LIBC", "libdl.so", &invalid));

    assert(get_error() == NULL);
    assert(open_library("libdoesnotexist.so", 0) == NULL);
    assert(strstr(get_error(), "not available"));
    assert(get_error() == NULL); // Android dlerror clears the pending error.

    void *log_library = open_library("liblog.so", 0);
    assert(log_library);
    assert(open_library("liblog.so", 4) == log_library); // RTLD_NOLOAD on active module.
    assert(close_library(log_library) == 0);
    uint64_t expected = 0;
    assert(android_log_compat_resolve("__android_log_write", &expected, NULL));
    assert(lookup_symbol(log_library, "__android_log_write") ==
           (void *)(uintptr_t)expected);
    assert(lookup_symbol(log_library, "not_a_log_symbol") == NULL);
    assert(strstr(get_error(), "not implemented"));
    assert(close_library(log_library) == 0);
    assert(open_library("liblog.so", 4) == NULL); // Closed module isn't loaded.
    assert(strstr(get_error(), "not loaded"));
    assert(lookup_symbol(log_library, "__android_log_write") == NULL);
    assert(strstr(get_error(), "closed"));
    assert(close_library(log_library) == -1);
    assert(strstr(get_error(), "closed"));

    void *android_library = open_library("/system/lib64/libandroid.so", 0);
    assert(android_library);
    assert(android_looper_compat_resolve("ALooper_prepare", NULL, NULL, &expected));
    assert(lookup_symbol(android_library, "ALooper_prepare") ==
           (void *)(uintptr_t)expected);
    assert(close_library(android_library) == 0);

    void *math_library = open_library("libm.so", 0);
    assert(math_library);
    assert(android_math_compat_resolve("sinf", "LIBC", "libm.so", &expected));
    assert(lookup_symbol(math_library, "sinf") == (void *)(uintptr_t)expected);
    assert(close_library(math_library) == 0);

    void *dl_library = open_library("libdl.so", 0);
    assert(dl_library);
    assert(lookup_symbol(dl_library, "dlopen") == (void *)(uintptr_t)open_library);
    assert(find_address((const void *)(uintptr_t)open_library, NULL) == 0);
    assert(close_library(dl_library) == 0);

    assert(open_library("/sdmc/surprise.so", 0) == NULL);
    pthread_t worker;
    assert(pthread_create(&worker, NULL, separate_thread, NULL) == 0);
    assert(pthread_join(worker, NULL) == 0);
    assert(strstr(get_error(), "path is unavailable")); // Thread-local errors.
    assert(get_error() == NULL);
    puts("android_dl_compat_test: all assertions passed");
}
