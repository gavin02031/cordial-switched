#define _POSIX_C_SOURCE 200809L
#include "android_libc_compat.h"
#include "android_stdio_compat.h"
#include "android_fortify_compat.h"

#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#include <wchar.h>
#include <arpa/inet.h>

static int call_checked_vsnprintf(uint64_t address, char *dest, size_t length,
                                  size_t capacity, const char *format, ...)
{
    int (*checked)(char *, size_t, int, size_t, const char *, va_list) =
        (int (*)(char *, size_t, int, size_t, const char *, va_list))(uintptr_t)address;
    va_list args;
    va_start(args, format);
    int result = checked(dest, length, 0, capacity, format, args);
    va_end(args);
    return result;
}

static int call_checked_vsprintf(uint64_t address, char *dest, size_t capacity,
                                 const char *format, ...)
{
    int (*checked)(char *, int, size_t, const char *, va_list) =
        (int (*)(char *, int, size_t, const char *, va_list))(uintptr_t)address;
    va_list args;
    va_start(args, format);
    int result = checked(dest, 0, capacity, format, args);
    va_end(args);
    return result;
}

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

    temporary = tmpfile();
    assert(temporary && fputs("item=42\n", temporary) >= 0);
    rewind(temporary);
    assert(android_stdio_compat_resolve("fgets", "LIBC", "libc.so", &address));
    char *(*android_fgets)(char *, int, FILE *) =
        (char *(*)(char *, int, FILE *))(uintptr_t)address;
    assert(android_fgets(buffer, sizeof(buffer), temporary) == buffer);
    assert(strcmp(buffer, "item=42\n") == 0);
    rewind(temporary);
    assert(android_stdio_compat_resolve("fscanf", "LIBC", "libc.so", &address));
    int (*android_fscanf)(FILE *, const char *, ...) =
        (int (*)(FILE *, const char *, ...))(uintptr_t)address;
    int parsed = 0;
    assert(android_fscanf(temporary, "item=%d", &parsed) == 1 && parsed == 42);
    assert(android_stdio_compat_resolve("getc", "LIBC", "libc.so", &address));
    int (*android_getc)(FILE *) = (int (*)(FILE *))(uintptr_t)address;
    assert(android_stdio_compat_resolve("ungetc", "LIBC", "libc.so", &address));
    int (*android_ungetc)(int, FILE *) =
        (int (*)(int, FILE *))(uintptr_t)address;
    assert(android_ungetc('X', temporary) == 'X');
    assert(android_getc(temporary) == 'X');
    assert(android_stdio_compat_resolve("ferror", "LIBC", "libc.so", &address));
    int (*android_ferror)(FILE *) = (int (*)(FILE *))(uintptr_t)address;
    assert(android_ferror(temporary) == 0);
    assert(android_stdio_compat_resolve("clearerr", "LIBC", "libc.so", &address));
    void (*android_clearerr)(FILE *) = (void (*)(FILE *))(uintptr_t)address;
    android_clearerr(temporary);
    assert(fclose(temporary) == 0);

    temporary = tmpfile();
    assert(temporary);
    assert(android_stdio_compat_resolve("fputwc", "LIBC", "libc.so", &address));
    wint_t (*android_fputwc)(wchar_t, FILE *) =
        (wint_t (*)(wchar_t, FILE *))(uintptr_t)address;
    assert(android_stdio_compat_resolve("getwc", "LIBC", "libc.so", &address));
    wint_t (*android_getwc)(FILE *) = (wint_t (*)(FILE *))(uintptr_t)address;
    assert(android_stdio_compat_resolve("ungetwc", "LIBC", "libc.so", &address));
    wint_t (*android_ungetwc)(wint_t, FILE *) =
        (wint_t (*)(wint_t, FILE *))(uintptr_t)address;
    assert(android_fputwc(L'A', temporary) == L'A');
    rewind(temporary);
    assert(android_getwc(temporary) == L'A');
    assert(android_ungetwc(L'B', temporary) == L'B');
    assert(android_getwc(temporary) == L'B');
    assert(fclose(temporary) == 0);

    // A legacy Android caller computes `&__sF[1]` using Bionic's 152-byte
    // stride. Those opaque bytes must never be passed straight to newlib.
    assert(android_stdio_compat_resolve("__sF", "LIBC", "libc.so", &address));
    assert(address != 0);
    FILE *legacy_stdin = (FILE *)(uintptr_t)address;
    FILE *legacy_stdout = (FILE *)(uintptr_t)(address + 152);
    FILE *legacy_stderr = (FILE *)(uintptr_t)(address + 304);
    assert(android_fileno(legacy_stdin) == fileno(stdin));
    assert(android_fileno(legacy_stdout) == fileno(stdout));
    assert(android_fileno(legacy_stderr) == fileno(stderr));
    assert(android_fflush(legacy_stdout) == 0);
    assert(android_fflush(legacy_stderr) == 0);
    assert(android_stdio_compat_resolve("fputs", "LIBC", "libc.so", &address));
    int (*android_fputs)(const char *, FILE *) =
        (int (*)(const char *, FILE *))(uintptr_t)address;
    assert(android_fputs("", legacy_stdout) >= 0);
    assert(android_stdio_compat_resolve("fclose", "LIBC", "libc.so", &address));
    int (*android_fclose)(FILE *) = (int (*)(FILE *))(uintptr_t)address;
    errno = 0;
    assert(android_fclose(legacy_stdout) == EOF && errno == EBADF);
    assert(android_fileno(legacy_stdout) == fileno(stdout));
    assert(android_stdio_compat_resolve("fseek", "LIBC", "libc.so", &address));
    assert(android_stdio_compat_resolve("ftell", "LIBC", "libc.so", &address));
    assert(android_stdio_compat_resolve("setvbuf", "LIBC", "libc.so", &address));

    // Never confuse Bionic's data symbols with the host's FILE storage or
    // permit a foreign provider/version to acquire our fake handles.
    assert(!android_stdio_compat_resolve("stderr", "LIBM", "libc.so", &address));
    assert(!android_stdio_compat_resolve("stderr", "LIBC", "libm.so", &address));
    assert(!android_stdio_compat_resolve("__sF", "LIBC_N", "libc.so", &address));
    assert(!android_stdio_compat_resolve("__sF", "LIBC", "libm.so", &address));
    assert(!android_stdio_compat_resolve("__fwrite_chk", "LIBC", "libc.so", &address));
    assert(!android_stdio_compat_resolve("stderr", "LIBC", "libc.so", NULL));

    assert(android_libc_compat_resolve("strlen", "LIBC", "libc.so", &address));
    size_t (*android_strlen)(const char *) =
        (size_t (*)(const char *))(uintptr_t)address;
    assert(android_strlen("Switch") == 6);
    assert(android_libc_compat_resolve("strtoll", "LIBC", "libc.so", &address));
    long long (*android_strtoll)(const char *, char **, int) =
        (long long (*)(const char *, char **, int))(uintptr_t)address;
    assert(android_strtoll("-9876543210", NULL, 10) == -9876543210LL);
    assert(android_libc_compat_resolve("strcspn", "LIBC", "libc.so", &address));
    size_t (*android_strcspn)(const char *, const char *) =
        (size_t (*)(const char *, const char *))(uintptr_t)address;
    assert(android_strcspn("world.txt", ".") == 5);
    assert(android_libc_compat_resolve("strcasecmp", "LIBC", "libc.so", &address));
    int (*android_strcasecmp)(const char *, const char *) =
        (int (*)(const char *, const char *))(uintptr_t)address;
    assert(android_strcasecmp("Roblox", "rObLoX") == 0);
    assert(android_libc_compat_resolve("wcslen", "LIBC", "libc.so", &address));
    size_t (*android_wcslen)(const wchar_t *) =
        (size_t (*)(const wchar_t *))(uintptr_t)address;
    assert(android_wcslen(L"Morrow") == 6);
    assert(android_libc_compat_resolve("wmemchr", "LIBC", "libc.so", &address));
    wchar_t *(*android_wmemchr)(const wchar_t *, wchar_t, size_t) =
        (wchar_t *(*)(const wchar_t *, wchar_t, size_t))(uintptr_t)address;
    assert(android_wmemchr(L"switch", L'i', 6) != NULL);
    assert(!android_libc_compat_resolve("wmemchr", "LIBC_N", "libc.so", &address));
    assert(android_libc_compat_resolve("in6addr_any", "LIBC_N", "libc.so", &address));
    const unsigned char *ipv6_any = (const unsigned char *)(uintptr_t)address;
    unsigned char zero_ipv6[16] = {0};
    assert(memcmp(ipv6_any, zero_ipv6, sizeof(zero_ipv6)) == 0);
    assert(android_libc_compat_resolve("in6addr_loopback", "LIBC_N", "libc.so", &address));
    const unsigned char *ipv6_loop = (const unsigned char *)(uintptr_t)address;
    assert(memcmp(ipv6_loop, zero_ipv6, sizeof(zero_ipv6) - 1) == 0);
    assert(ipv6_loop[15] == 1);
    assert(!android_libc_compat_resolve("in6addr_any", "LIBC", "libc.so", &address));
    assert(android_libc_compat_resolve("ldexp", "LIBC", "libc.so", &address));
    double (*android_ldexp)(double, int) = (double (*)(double, int))(uintptr_t)address;
    assert(android_ldexp(1.5, 3) == 12.0);
    assert(!android_libc_compat_resolve("ldexp", "LIBC", "libm.so", &address));
    assert(android_libc_compat_resolve("getentropy", NULL, NULL, &address));
    int (*android_getentropy)(void *, size_t) =
        (int (*)(void *, size_t))(uintptr_t)address;
    unsigned char random_buffer[32] = {0};
    assert(android_getentropy(random_buffer, sizeof(random_buffer)) == 0);
    // A successful call must produce actual random data, not an all-zero
    // placeholder. A 256-bit all-zero sample is negligibly likely.
    unsigned nonzero = 0;
    for (size_t i = 0; i < sizeof(random_buffer); i++)
        nonzero |= random_buffer[i];
    assert(nonzero != 0);
    errno = 0;
    assert(android_getentropy(random_buffer, 257) == -1 && errno == EIO);
    errno = 0;
    assert(android_getentropy(NULL, 1) == -1 && errno == EFAULT);
    assert(!android_libc_compat_resolve("getentropy", "LIBC", "libc.so", &address));
    assert(!android_libc_compat_resolve("getentropy", NULL, "libc.so", &address));
    assert(android_libc_compat_resolve("arc4random_buf", "LIBC", "libc.so", &address));
    void (*android_random_buf)(void *, size_t) =
        (void (*)(void *, size_t))(uintptr_t)address;
    unsigned char extended_random[384] = {0};
    android_random_buf(extended_random, sizeof(extended_random));
    nonzero = 0;
    for (size_t i = 0; i < sizeof(extended_random); i++)
        nonzero |= extended_random[i];
    assert(nonzero != 0);
    assert(android_libc_compat_resolve("__stack_chk_guard", "LIBC", "libc.so", &address));
    uintptr_t *canary = (uintptr_t *)(uintptr_t)address;
    assert(*canary != 0);
    assert(android_libc_compat_resolve("__stack_chk_guard", "LIBC", "libc.so", &address));
    assert((uintptr_t *)(uintptr_t)address == canary);
    assert(!android_libc_compat_resolve("__stack_chk_guard", "LIBC_N", "libc.so", &address));
    assert(android_libc_compat_resolve("memrchr", "LIBC", "libc.so", &address));
    void *(*android_memrchr)(const void *, int, size_t) =
        (void *(*)(const void *, int, size_t))(uintptr_t)address;
    const char *repeated = "abca";
    assert(android_memrchr(repeated, 'a', 4) == repeated + 3);
    assert(android_memrchr(repeated, 'z', 4) == NULL);
    assert(android_libc_compat_resolve("getpagesize", "LIBC", "libc.so", &address));
    int (*android_pagesize)(void) = (int (*)(void))(uintptr_t)address;
    assert(android_pagesize() >= 4096);
    assert(android_libc_compat_resolve("sched_yield", "LIBC", "libc.so", &address));
    int (*android_yield)(void) = (int (*)(void))(uintptr_t)address;
    assert(android_yield() == 0);
    assert(android_libc_compat_resolve("btowc", "LIBC", "libc.so", &address));
    wint_t (*android_btowc)(int) = (wint_t (*)(int))(uintptr_t)address;
    assert(android_btowc('A') == L'A');
    assert(android_libc_compat_resolve("wctob", "LIBC", "libc.so", &address));
    int (*android_wctob)(wint_t) = (int (*)(wint_t))(uintptr_t)address;
    assert(android_wctob(L'B') == 'B');
    assert(android_libc_compat_resolve("sscanf", "LIBC", "libc.so", &address));
    int (*android_sscanf)(const char *, const char *, ...) =
        (int (*)(const char *, const char *, ...))(uintptr_t)address;
    int scanned = 0;
    assert(android_sscanf("value=81", "value=%d", &scanned) == 1 && scanned == 81);
    assert(android_libc_compat_resolve("time", "LIBC", "libc.so", &address));
    time_t (*android_time)(time_t *) = (time_t (*)(time_t *))(uintptr_t)address;
    assert(android_time(NULL) > 0);
    assert(android_libc_compat_resolve("nanosleep", "LIBC", "libc.so", &address));
    int (*android_sleep)(const struct timespec *, struct timespec *) =
        (int (*)(const struct timespec *, struct timespec *))(uintptr_t)address;
    const struct timespec no_sleep = {0};
    assert(android_sleep(&no_sleep, NULL) == 0);
    assert(android_libc_compat_resolve("getenv", "LIBC", "libc.so", &address));
    char *(*android_getenv)(const char *) =
        (char *(*)(const char *))(uintptr_t)address;
    assert(android_getenv("MORROW_TEST_VARIABLE_DOES_NOT_EXIST") == NULL);
    for (size_t i = 0; i < 3; i++) {
        const char *names[] = {"usleep", "gettimeofday", "exit"};
        assert(android_libc_compat_resolve(names[i], "LIBC", "libc.so", &address));
    }
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
    assert(android_fortify_compat_resolve("__mempcpy_chk", "LIBC", "libc.so", &address));
    void *(*checked_mempcpy)(void *, const void *, size_t, size_t) =
        (void *(*)(void *, const void *, size_t, size_t))(uintptr_t)address;
    char end_pointer[8] = {0};
    assert(checked_mempcpy(end_pointer, "hi", 3, sizeof(end_pointer)) == end_pointer + 3);
    assert(checked_mempcpy(end_pointer + 2, "!", 2, sizeof(end_pointer) - 2)
           == end_pointer + 4);
    assert(strcmp(end_pointer, "hi!") == 0);
    assert(checked_mempcpy(end_pointer, "", 0, 0) == end_pointer);
    assert(!android_fortify_compat_resolve("__mempcpy_chk", "LIBC_N", "libc.so", &address));
    assert(!android_fortify_compat_resolve("__mempcpy_chk", "LIBC", "libm.so", &address));
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
    assert(android_fortify_compat_resolve("__strcpy_chk", "LIBC", "libc.so", &address));
    char *(*checked_strcpy)(char *, const char *, size_t) =
        (char *(*)(char *, const char *, size_t))(uintptr_t)address;
    // Source is only three bytes long; a larger destination bound must not
    // be used as a source scan limit (ASan catches that regression).
    char short_source[3] = {'o', 'k', 0};
    char copied[32] = {0};
    assert(checked_strcpy(copied, short_source, sizeof(copied)) == copied);
    assert(strcmp(copied, "ok") == 0);
    assert(checked_strcpy(copied, short_source, 3) == copied);
    assert(!android_fortify_compat_resolve("__strcpy_chk", "LIBC_N", "libc.so", &address));
    assert(!android_fortify_compat_resolve("__strcpy_chk", "LIBC", "libm.so", &address));
    assert(android_fortify_compat_resolve("__strcat_chk", "LIBC", "libc.so", &address));
    char *(*checked_strcat)(char *, const char *, size_t) =
        (char *(*)(char *, const char *, size_t))(uintptr_t)address;
    char appended[12] = "ab";
    assert(checked_strcat(appended, "cd", sizeof(appended)) == appended);
    assert(strcmp(appended, "abcd") == 0);
    assert(checked_strcat(appended, "1234567", sizeof(appended)) == appended);
    assert(strcmp(appended, "abcd1234567") == 0);
    assert(!android_fortify_compat_resolve("__strcat_chk", "LIBC_N", "libc.so", &address));
    assert(!android_fortify_compat_resolve("__strcat_chk", "LIBC", "libm.so", &address));
    assert(android_fortify_compat_resolve("__strncat_chk", "LIBC", "libc.so", &address));
    char *(*checked_strncat)(char *, const char *, size_t, size_t) =
        (char *(*)(char *, const char *, size_t, size_t))(uintptr_t)address;
    char limited[8] = "ab";
    assert(checked_strncat(limited, "cdef", 2, sizeof(limited)) == limited);
    assert(strcmp(limited, "abcd") == 0);
    assert(checked_strncat(limited, "efg", 3, sizeof(limited)) == limited);
    assert(strcmp(limited, "abcdefg") == 0);
    assert(checked_strncat(limited, "ignored", 0, sizeof(limited)) == limited);
    assert(!android_fortify_compat_resolve("__strncat_chk", "LIBC_N", "libc.so", &address));
    assert(!android_fortify_compat_resolve("__strncat_chk", "LIBC", "libm.so", &address));
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
    assert(android_fortify_compat_resolve("__strchr_chk", "LIBC", "libc.so", &address));
    char *(*checked_strchr)(const char *, int, size_t) =
        (char *(*)(const char *, int, size_t))(uintptr_t)address;
    assert(checked_strchr("morrow", 'r', 7) == strchr("morrow", 'r'));
    assert(!android_fortify_compat_resolve("__strchr_chk", "LIBC_N", "libc.so", &address));
    assert(android_fortify_compat_resolve("__vsnprintf_chk", "LIBC", "libc.so", &address));
    uint64_t checked_vsnprintf = address;
    char formatted[16] = {0};
    assert(call_checked_vsnprintf(checked_vsnprintf, formatted, sizeof(formatted),
                                  sizeof(formatted), "v%d", 7) == 2);
    assert(strcmp(formatted, "v7") == 0);
    assert(android_fortify_compat_resolve("__vsprintf_chk", "LIBC", "libc.so", &address));
    uint64_t checked_vsprintf = address;
    assert(call_checked_vsprintf(checked_vsprintf, formatted, sizeof(formatted),
                                 "hello%d", 5) == 6);
    assert(strcmp(formatted, "hello5") == 0);
    assert(android_fortify_compat_resolve("__stack_chk_fail", "LIBC", "libc.so", &address));

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
        char unterminated_text[3] = {'a', 'b', 'c'};
        checked_strchr(unterminated_text, 'z', sizeof(unterminated_text));
        _exit(99);
    }
    assert(waitpid(child, &status, 0) == child);
    assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT);

    child = fork();
    assert(child >= 0);
    if (child == 0) {
        call_checked_vsnprintf(checked_vsnprintf, formatted, 17, 16, "too big");
        _exit(99);
    }
    assert(waitpid(child, &status, 0) == child);
    assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT);

    child = fork();
    assert(child >= 0);
    if (child == 0) {
        call_checked_vsprintf(checked_vsprintf, formatted, 4, "overflow");
        _exit(99);
    }
    assert(waitpid(child, &status, 0) == child);
    assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT);

    child = fork();
    assert(child >= 0);
    if (child == 0) {
        checked_mempcpy(destination, "unsafe", 7, 2);
        _exit(99);
    }
    assert(waitpid(child, &status, 0) == child);
    assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT);

    child = fork();
    assert(child >= 0);
    if (child == 0) {
        checked_strcpy(copied, "overflow", 3);
        _exit(99);
    }
    assert(waitpid(child, &status, 0) == child);
    assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT);

    child = fork();
    assert(child >= 0);
    if (child == 0) {
        checked_strcat(appended, "x", sizeof(appended));
        _exit(99);
    }
    assert(waitpid(child, &status, 0) == child);
    assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT);

    child = fork();
    assert(child >= 0);
    if (child == 0) {
        char unterminated[4] = {'a', 'b', 'c', 'd'};
        checked_strcat(unterminated, "", sizeof(unterminated));
        _exit(99);
    }
    assert(waitpid(child, &status, 0) == child);
    assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT);

    child = fork();
    assert(child >= 0);
    if (child == 0) {
        checked_strncat(limited, "x", 1, sizeof(limited));
        _exit(99);
    }
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

    // These are real POSIX file/time functions. Avoid claiming support for
    // Android stat layouts, path translation or opaque libc structures.
    const char *portable_libc[] = {
        "getcwd", "access", "remove", "rename", "rmdir", "unlink",
        "readlink", "mkdir", "realpath", "close", "read", "write",
        "fsync", "strerror", "gmtime", "gmtime_r", "strftime",
        "localtime", "localtime_r", "mktime", "clock", "tzset",
        "inet_pton", "inet_ntop", "getpid"
    };
    for (size_t i = 0; i < sizeof(portable_libc) / sizeof(portable_libc[0]); i++) {
        assert(android_libc_compat_resolve(portable_libc[i], "LIBC", "libc.so", &address));
        assert(address != 0);
        assert(!android_libc_compat_resolve(portable_libc[i], "LIBC_N", "libc.so", &address));
    }
    assert(!android_libc_compat_resolve("stat", "LIBC", "libc.so", &address));
    assert(!android_libc_compat_resolve("longjmp", "LIBC", "libc.so", &address));

    assert(android_libc_compat_resolve("inet_pton", "LIBC", "libc.so", &address));
    int (*android_inet_pton)(int, const char *, void *) =
        (int (*)(int, const char *, void *))(uintptr_t)address;
    unsigned char parsed_ipv4[4] = {0};
    assert(android_inet_pton(AF_INET, "127.0.0.1", parsed_ipv4) == 1);
    assert(parsed_ipv4[0] == 127 && parsed_ipv4[3] == 1);
    assert(android_libc_compat_resolve("inet_ntop", "LIBC", "libc.so", &address));
    const char *(*android_inet_ntop)(int, const void *, char *, socklen_t) =
        (const char *(*)(int, const void *, char *, socklen_t))(uintptr_t)address;
    char ip_text[32] = {0};
    assert(android_inet_ntop(AF_INET, parsed_ipv4, ip_text, sizeof(ip_text)) == ip_text);
    assert(strcmp(ip_text, "127.0.0.1") == 0);

    assert(android_libc_compat_resolve("getpid", "LIBC", "libc.so", &address));
    pid_t (*android_getpid)(void) = (pid_t (*)(void))(uintptr_t)address;
    assert(android_getpid() == getpid());
    assert(android_libc_compat_resolve("gmtime", "LIBC", "libc.so", &address));
    struct tm *(*android_gmtime)(const time_t *) =
        (struct tm *(*)(const time_t *))(uintptr_t)address;
    time_t epoch = 0;
    assert(android_gmtime(&epoch)->tm_year == 70);

    // Exercise the checked read/write entry points with a real pipe. A
    // versioned import under LIBC_N must not bind a generic LIBC symbol.
    assert(android_fortify_compat_resolve("__write_chk", "LIBC_N", "libc.so", &address));
    ssize_t (*checked_write)(int, const void *, size_t, size_t) =
        (ssize_t (*)(int, const void *, size_t, size_t))(uintptr_t)address;
    assert(!android_fortify_compat_resolve("__write_chk", "LIBC", "libc.so", &address));
    assert(android_fortify_compat_resolve("__read_chk", "LIBC", "libc.so", &address));
    ssize_t (*checked_read)(int, void *, size_t, size_t) =
        (ssize_t (*)(int, void *, size_t, size_t))(uintptr_t)address;
    assert(!android_fortify_compat_resolve("__read_chk", "LIBC_N", "libc.so", &address));
    int descriptors[2] = {-1, -1};
    assert(pipe(descriptors) == 0);
    assert(checked_write(descriptors[1], "hello", 5, 5) == 5);
    char read_back[6] = {0};
    assert(checked_read(descriptors[0], read_back, 5, 5) == 5);
    assert(strcmp(read_back, "hello") == 0);
    assert(close(descriptors[0]) == 0 && close(descriptors[1]) == 0);
    child = fork();
    assert(child >= 0);
    if (child == 0) {
        checked_write(-1, "overflow", 8, 2);
        _exit(99);
    }
    assert(waitpid(child, &status, 0) == child);
    assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT);

    assert(android_fortify_compat_resolve("__readlink_chk", "LIBC", "libc.so", &address));
    ssize_t (*checked_readlink)(const char *, char *, size_t, size_t) =
        (ssize_t (*)(const char *, char *, size_t, size_t))(uintptr_t)address;
    errno = 0;
    assert(checked_readlink("/a/nonexistent/target", read_back, 5, 5) == -1);
    assert(errno == ENOENT || errno == ENOTDIR);
    child = fork();
    assert(child >= 0);
    if (child == 0) {
        checked_readlink("ignored", read_back, 6, 2);
        _exit(99);
    }
    assert(waitpid(child, &status, 0) == child);
    assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT);

    puts("android_platform_compat_test: Bionic stdio, libc and fortify adapters passed");
    return 0;
}
