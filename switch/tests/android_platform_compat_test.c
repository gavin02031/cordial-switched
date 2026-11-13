#define _POSIX_C_SOURCE 200809L
#include "android_libc_compat.h"
#include "android_stdio_compat.h"

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

int main(void)
{
    uint64_t address = 0;
    assert(android_stdio_compat_resolve("stderr", "LIBC", "libc.so", &address));
    FILE *android_stderr = *(FILE **)(uintptr_t)address;
    assert(android_stderr && android_stderr != stderr);

    assert(android_stdio_compat_resolve("fileno", "LIBC", "libc.so", &address));
    int (*android_fileno)(FILE *) = (int (*)(FILE *))(uintptr_t)address;
    assert(android_fileno(android_stderr) == fileno(stderr));

    assert(android_stdio_compat_resolve("fflush", "LIBC", "libc.so", &address));
    int (*android_fflush)(FILE *) = (int (*)(FILE *))(uintptr_t)address;
    assert(android_fflush(android_stderr) == 0);

    FILE *temporary = tmpfile();
    assert(temporary);
    assert(android_stdio_compat_resolve("fprintf", "LIBC", "libc.so", &address));
    int (*android_fprintf)(FILE *, const char *, ...) =
        (int (*)(FILE *, const char *, ...))(uintptr_t)address;
    assert(android_fprintf(temporary, "value=%d", 17) == 8);
    assert(android_fflush(temporary) == 0);
    rewind(temporary);
    char buffer[32] = {0};
    assert(fread(buffer, 1, 8, temporary) == 8);
    assert(strcmp(buffer, "value=17") == 0);
    assert(fclose(temporary) == 0);

    // Never confuse Bionic's data symbol with the host's FILE storage or
    // permit a foreign provider/version to acquire our fake handles.
    assert(!android_stdio_compat_resolve("stderr", "LIBM", "libc.so", &address));
    assert(!android_stdio_compat_resolve("stderr", "LIBC", "libm.so", &address));
    assert(!android_stdio_compat_resolve("__sF", "LIBC", "libc.so", &address));
    assert(!android_stdio_compat_resolve("__fwrite_chk", "LIBC", "libc.so", &address));
    assert(!android_stdio_compat_resolve("stderr", "LIBC", "libc.so", NULL));

    assert(android_libc_compat_resolve("strlen", "LIBC", "libc.so", &address));
    size_t (*android_strlen)(const char *) =
        (size_t (*)(const char *))(uintptr_t)address;
    assert(android_strlen("Switch") == 6);
    assert(android_libc_compat_resolve("clock_gettime", "LIBC", "libc.so", &address));
    int (*android_clock_gettime)(int, struct timespec *) =
        (int (*)(int, struct timespec *))(uintptr_t)address;
    struct timespec time_value = {0};
    assert(android_clock_gettime(0, &time_value) == 0);
    assert(time_value.tv_sec > 0);
    assert(android_clock_gettime(1, &time_value) == 0);
    errno = 0;
    assert(android_clock_gettime(99, &time_value) == -1 && errno == EINVAL);
    assert(!android_libc_compat_resolve("clock_gettime", "LIBC", "libm.so", &address));
    assert(!android_libc_compat_resolve("pthread_mutex_lock", "LIBC", "libc.so", &address));
    assert(!android_libc_compat_resolve("__errno", "LIBC", "libc.so", &address));

    puts("android_platform_compat_test: Bionic stdio and libc adapters passed");
    return 0;
}
