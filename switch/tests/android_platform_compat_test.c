#define _POSIX_C_SOURCE 200809L
#include "android_libc_compat.h"
#include "android_stdio_compat.h"
#include "android_fortify_compat.h"

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

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

    // Bionic's fortify functions must perform real size checks instead of
    // pretending that a possibly unsafe copy succeeded.
    assert(android_fortify_compat_resolve("__memcpy_chk", "LIBC", "libc.so", &address));
    void *(*checked_memcpy)(void *, const void *, size_t, size_t) =
        (void *(*)(void *, const void *, size_t, size_t))(uintptr_t)address;
    char destination[16] = {0};
    assert(checked_memcpy(destination, "safe", 5, sizeof(destination)) == destination);
    assert(strcmp(destination, "safe") == 0);
    assert(android_fortify_compat_resolve("__memmove_chk", "LIBC", "libc.so", &address));
    void *(*checked_memmove)(void *, const void *, size_t, size_t) =
        (void *(*)(void *, const void *, size_t, size_t))(uintptr_t)address;
    char overlapping[16] = "abcdef";
    assert(checked_memmove(overlapping + 2, overlapping, 6, sizeof(overlapping) - 2)
           == overlapping + 2);
    assert(strcmp(overlapping, "ababcdef") == 0);
    assert(android_fortify_compat_resolve("__strlen_chk", "LIBC", "libc.so", &address));
    size_t (*checked_strlen)(const char *, size_t) =
        (size_t (*)(const char *, size_t))(uintptr_t)address;
    assert(checked_strlen(destination, 5) == 4);
    assert(android_fortify_compat_resolve("__strncpy_chk2", "LIBC", "libc.so", &address));
    char *(*checked_strncpy2)(char *, const char *, size_t, size_t, size_t) =
        (char *(*)(char *, const char *, size_t, size_t, size_t))(uintptr_t)address;
    char dest2[8] = {0};
    assert(checked_strncpy2(dest2, "a", 5, sizeof(dest2), 2) == dest2);
    assert(dest2[0] == 'a' && dest2[1] == 0 && dest2[4] == 0);
    assert(android_fortify_compat_resolve("__errno", "LIBC", "libc.so", &address));
    int *(*android_errno)(void) = (int *(*)(void))(uintptr_t)address;
    errno = EAGAIN;
    assert(android_errno() == &errno && *android_errno() == EAGAIN);
    assert(!android_fortify_compat_resolve("__memcpy_chk", "LIBC_N", "libc.so", &address));
    assert(!android_fortify_compat_resolve("pthread_mutex_lock", "LIBC", "libc.so", &address));

    assert(android_stdio_compat_resolve("__fwrite_chk", "LIBC_N", "libc.so", &address));
    size_t (*checked_fwrite)(const void *, size_t, size_t, FILE *, size_t) =
        (size_t (*)(const void *, size_t, size_t, FILE *, size_t))(uintptr_t)address;
    temporary = tmpfile();
    assert(temporary);
    assert(checked_fwrite("test", 1, 4, temporary, 4) == 4);
    assert(fclose(temporary) == 0);
    assert(!android_stdio_compat_resolve("__fwrite_chk", "LIBC", "libc.so", &address));
    assert(!android_stdio_compat_resolve("fwrite", "LIBC_N", "libc.so", &address));

    // The host harness can isolate intentional fatal failures in children.
    // A failure must abort, not copy an out-of-bounds buffer and return.
    struct rlimit no_core = {.rlim_cur = 0, .rlim_max = 0};
    assert(setrlimit(RLIMIT_CORE, &no_core) == 0);
    pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
        checked_memcpy(destination, "unsafe", 7, 2);
        _exit(99);
    }
    int status = 0;
    assert(waitpid(child, &status, 0) == child);
    assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT);

    child = fork();
    assert(child >= 0);
    if (child == 0) {
        checked_memmove(overlapping + 2, overlapping, 15, 14);
        _exit(99);
    }
    assert(waitpid(child, &status, 0) == child);
    assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT);

    child = fork();
    assert(child >= 0);
    if (child == 0) {
        checked_fwrite("test", 1, 4, stdout, 2);
        _exit(99);
    }
    assert(waitpid(child, &status, 0) == child);
    assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT);

    puts("android_platform_compat_test: Bionic stdio, libc and fortify adapters passed");
    return 0;
}
