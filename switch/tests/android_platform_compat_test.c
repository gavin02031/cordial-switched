#define _POSIX_C_SOURCE 200809L
#include "android_libc_compat.h"
#include "android_stdio_compat.h"
#include "android_fortify_compat.h"

#include <assert.h>
#include <pthread.h>
#include <getopt.h>
#include <limits.h>
#include <locale.h>
#include <stdlib.h>
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

static int call_android_vasprintf(uint64_t address, char **output,
                                 const char *format, ...)
{
    int (*function)(char **, const char *, va_list) =
        (int (*)(char **, const char *, va_list))(uintptr_t)address;
    va_list arguments;
    va_start(arguments, format);
    int result = function(output, format, arguments);
    va_end(arguments);
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

static int exit_marker_fd = -1;

static void unexpected_exit_handler(void)
{
    const char marker = 'x';
    if (write(exit_marker_fd, &marker, 1) != 1) _Exit(98);
}

static void test_error_strings_and_immediate_exit(void)
{
    uint64_t address = 0;
    assert(android_libc_compat_resolve("strerror_r", "LIBC", "libc.so", &address));
    int (*error_string)(int, char *, size_t) =
        (int (*)(int, char *, size_t))(uintptr_t)address;
    char buffer[80];
    errno = EBUSY;
    assert(error_string(0, buffer, sizeof(buffer)) == 0 && !strcmp(buffer, "Success"));
    assert(error_string(11, buffer, sizeof(buffer)) == 0 && !strcmp(buffer, "Try again"));
    // These guest numbers differ from the Horizon/newlib values.
    assert(error_string(95, buffer, sizeof(buffer)) == 0 &&
           !strcmp(buffer, "Operation not supported on transport endpoint"));
    assert(error_string(115, buffer, sizeof(buffer)) == 0 &&
           !strcmp(buffer, "Operation now in progress"));
    assert(error_string(133, buffer, sizeof(buffer)) == 0 &&
           !strcmp(buffer, "Memory page has hardware error"));
    assert(error_string(41, buffer, sizeof(buffer)) == 0 && !strcmp(buffer, "Unknown error 41"));
    assert(error_string(INT_MIN, buffer, sizeof(buffer)) == 0 &&
           !strcmp(buffer, "Unknown error -2147483648"));
    assert(error_string(INT_MAX, buffer, sizeof(buffer)) == 0 &&
           !strcmp(buffer, "Unknown error 2147483647"));
    assert(error_string(0, buffer, 8) == 0 && !strcmp(buffer, "Success"));
    buffer[7] = 'x';
    assert(error_string(0, buffer, 7) == 34 && !strcmp(buffer, "Succes") && buffer[7] == 'x');
    assert(error_string(0, buffer, 1) == 34 && buffer[0] == 0);
    buffer[0] = 'y';
    assert(error_string(0, buffer, 0) == 34 && buffer[0] == 'y');
    assert(error_string(0, NULL, 0) == 34);
    assert(errno == EBUSY);
    assert(android_libc_compat_resolve("__gnu_strerror_r", "LIBC", "libc.so", &address));
    char *(*gnu_error_string)(int, char *, size_t) =
        (char *(*)(int, char *, size_t))(uintptr_t)address;
    assert(gnu_error_string(0, buffer, sizeof(buffer)) == buffer && !strcmp(buffer, "Success"));
    assert(gnu_error_string(0, buffer, 3) == buffer && !strcmp(buffer, "Su"));
    assert(gnu_error_string(0, buffer, 0) == buffer && !strcmp(buffer, "Su"));
    assert(errno == EBUSY);
    const char *names[] = {"strerror_r", "__gnu_strerror_r", "_exit"};
    for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        assert(!android_libc_compat_resolve(names[i], "LIBC_N", "libc.so", &address));
        assert(!android_libc_compat_resolve(names[i], "LIBC", "libdl.so", &address));
        assert(!android_libc_compat_resolve(names[i], NULL, NULL, &address));
    }
    assert(android_libc_compat_resolve("_exit", "LIBC", "libc.so", &address));
    void (*immediate_exit)(int) = (void (*)(int))(uintptr_t)address;
    int descriptors[2];
    assert(pipe(descriptors) == 0);
    pid_t child = fork();
    assert(child >= 0);
    if (!child) {
        close(descriptors[0]);
        exit_marker_fd = descriptors[1];
        assert(atexit(unexpected_exit_handler) == 0);
        immediate_exit(37);
        abort();
    }
    close(descriptors[1]);
    int status = 0;
    assert(waitpid(child, &status, 0) == child);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 37);
    assert(read(descriptors[0], buffer, sizeof(buffer)) == 0);
    close(descriptors[0]);
}

static int32_t (*guest_gettid)(void);

static void *tid_worker(void *opaque)
{
    int32_t *result = opaque;
    *result = guest_gettid();
    assert(*result > 0 && *result == guest_gettid());
    return NULL;
}

int main(void)
{
    test_error_strings_and_immediate_exit();
    uint64_t address = 0;
    assert(android_stdio_compat_resolve("vasprintf", "LIBC", "libc.so", &address));
    char *allocated_text = NULL;
    assert(call_android_vasprintf(address, &allocated_text, "value=%d:%s", 27, "ok") == 11);
    assert(allocated_text && !strcmp(allocated_text, "value=27:ok"));
    free(allocated_text);
    allocated_text = (char *)(uintptr_t)0x1;
    assert(call_android_vasprintf(address, &allocated_text, NULL) == -1 &&
           errno == EINVAL && allocated_text == (char *)(uintptr_t)0x1);
    assert(call_android_vasprintf(address, NULL, "%s", "nope") == -1 && errno == EINVAL);
    assert(!android_stdio_compat_resolve("vasprintf", "LIBC_N", "libc.so", &address));
    assert(!android_libc_compat_resolve("geteuid", "LIBC", "libc.so", &address));
    assert(android_libc_compat_resolve("localeconv", "LIBC", "libc.so", &address));
    struct lconv *(*android_localeconv)(void) =
        (struct lconv *(*)(void))(uintptr_t)address;
    struct lconv *numeric = android_localeconv();
    assert(numeric && strcmp(numeric->decimal_point, ".") == 0 &&
           !numeric->thousands_sep[0] && numeric->int_frac_digits == CHAR_MAX &&
           numeric->int_n_sign_posn == CHAR_MAX && android_localeconv() == numeric);
    assert(!android_libc_compat_resolve("localeconv", "LIBC", "libm.so", &address));

    typedef struct {
        void *name; uint32_t name_length; void *iov; size_t iov_length;
        void *control; size_t control_length; int flags;
    } GuestMessage;
    typedef struct { size_t length; int level, type; } GuestControl;
    assert(android_libc_compat_resolve("__cmsg_nxthdr", "LIBC", "libc.so", &address));
    GuestControl *(*next_control)(const GuestMessage *, const GuestControl *) =
        (GuestControl *(*)(const GuestMessage *, const GuestControl *))(uintptr_t)address;
    _Alignas(8) unsigned char controls[64] = {0};
    GuestMessage message = {.control = controls, .control_length = sizeof(controls)};
    GuestControl *first = (GuestControl *)controls;
    first->length = 19; // 24-byte aligned stride.
    GuestControl *second = (GuestControl *)(controls + 24);
    second->length = 16;
    assert(next_control(&message, first) == second);
    assert(next_control(&message, second) == (GuestControl *)(controls + 40));
    message.control_length = 39;
    assert(next_control(&message, first) == NULL);
    message.control_length = sizeof(controls);
    first->length = SIZE_MAX;
    assert(next_control(&message, first) == NULL);
    first->length = 8;
    assert(next_control(&message, first) == NULL);
    first->length = 19;
    assert(next_control(&message, (GuestControl *)(controls + 60)) == NULL);
    assert(next_control(NULL, first) == NULL);
    assert(!android_libc_compat_resolve("__cmsg_nxthdr", "LIBC_N", "libc.so", &address));

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
    // Android's checked fd_set functions use exactly 1024 descriptor bits,
    // independent of the native libc's fd_set layout.
    assert(android_fortify_compat_resolve("__FD_SET_chk", "LIBC", "libc.so", &address));
    void (*checked_fd_set)(int, void *, size_t) =
        (void (*)(int, void *, size_t))(uintptr_t)address;
    assert(android_fortify_compat_resolve("__FD_ISSET_chk", "LIBC", "libc.so", &address));
    int (*checked_fd_isset)(int, const void *, size_t) =
        (int (*)(int, const void *, size_t))(uintptr_t)address;
    assert(android_fortify_compat_resolve("__FD_CLR_chk", "LIBC", "libc.so", &address));
    void (*checked_fd_clr)(int, void *, size_t) =
        (void (*)(int, void *, size_t))(uintptr_t)address;
    assert(!android_fortify_compat_resolve("__FD_SET_chk", "LIBC_O", "libc.so", &address));
    assert(!android_fortify_compat_resolve("__FD_SET_chk", "LIBC", "libm.so", &address));
    unsigned char guest_fds[128] = {0};
    checked_fd_set(0, guest_fds, sizeof(guest_fds));
    checked_fd_set(63, guest_fds, sizeof(guest_fds));
    checked_fd_set(1023, guest_fds, sizeof(guest_fds));
    assert(checked_fd_isset(0, guest_fds, sizeof(guest_fds)) == 1);
    assert(checked_fd_isset(63, guest_fds, sizeof(guest_fds)) == 1);
    assert(checked_fd_isset(64, guest_fds, sizeof(guest_fds)) == 0);
    assert(checked_fd_isset(1023, guest_fds, sizeof(guest_fds)) == 1);
    checked_fd_clr(63, guest_fds, sizeof(guest_fds));
    assert(checked_fd_isset(63, guest_fds, sizeof(guest_fds)) == 0);
    pid_t fd_failure = fork();
    assert(fd_failure >= 0);
    if (!fd_failure) { checked_fd_set(-1, guest_fds, sizeof(guest_fds)); _exit(20); }
    int fd_status;
    assert(waitpid(fd_failure, &fd_status, 0) == fd_failure);
    assert(WIFSIGNALED(fd_status) && WTERMSIG(fd_status) == SIGABRT);
    fd_failure = fork();
    assert(fd_failure >= 0);
    if (!fd_failure) { checked_fd_clr(1024, guest_fds, sizeof(guest_fds)); _exit(21); }
    assert(waitpid(fd_failure, &fd_status, 0) == fd_failure);
    assert(WIFSIGNALED(fd_status) && WTERMSIG(fd_status) == SIGABRT);
    fd_failure = fork();
    assert(fd_failure >= 0);
    if (!fd_failure) { (void)checked_fd_isset(8, guest_fds, 16); _exit(22); }
    assert(waitpid(fd_failure, &fd_status, 0) == fd_failure);
    assert(WIFSIGNALED(fd_status) && WTERMSIG(fd_status) == SIGABRT);
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

    uint64_t guarded_set[] = {17, 123, 99};
    const char *set_names[] = {"sigfillset", "sigemptyset"};
    for (unsigned i = 0; i < 2; ++i) {
        assert(android_libc_compat_resolve(set_names[i], "LIBC", "libc.so", &address));
        int (*set_fn)(void *) = (void *)(uintptr_t)address;
        assert(set_fn(&guarded_set[1]) == 0);
        assert(guarded_set[1] == (i == 0 ? UINT64_MAX : 0));
        assert(guarded_set[0] == 17 && guarded_set[2] == 99);
        assert(set_fn(NULL) == -1 && errno == EINVAL);
        assert(!android_libc_compat_resolve(set_names[i], "LIBC_N", "libc.so", &address));
    }
    assert(android_libc_compat_resolve("getpid", "LIBC", "libc.so", &address));
    pid_t (*android_getpid)(void) = (pid_t (*)(void))(uintptr_t)address;
    assert(android_getpid() == getpid());
    assert(android_libc_compat_resolve("gettid", "LIBC", "libc.so", &address));
    guest_gettid = (int32_t (*)(void))(uintptr_t)address;
    assert(guest_gettid() == getpid()); // Linux main thread owns the PID.
    pthread_t tid_thread;
    int32_t worker_tid = 0;
    assert(pthread_create(&tid_thread, NULL, tid_worker, &worker_tid) == 0);
    assert(pthread_join(tid_thread, NULL) == 0);
    assert(worker_tid != guest_gettid());
    assert(!android_libc_compat_resolve("gettid", "LIBC_N", "libc.so", &address));

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

    // ELF data imports must alias live host globals, not copied initial values.
    assert(android_libc_compat_resolve("optarg", "LIBC", "libc.so", &address));
    char **guest_optarg = (char **)(uintptr_t)address;
    assert(android_libc_compat_resolve("optind", "LIBC", "libc.so", &address));
    int *guest_optind = (int *)(uintptr_t)address;
    assert(android_libc_compat_resolve("getopt_long", "LIBC", "libc.so", &address));
    int (*guest_getopt)(int, char *const *, const char *, const struct option *, int *) =
        (int (*)(int, char *const *, const char *, const struct option *, int *))(uintptr_t)address;
    char *arguments[] = {"morrow", "--name", "example", NULL};
    struct option options[] = {{"name", required_argument, NULL, 'n'}, {NULL, 0, NULL, 0}};
    *guest_optind = 1;
    assert(guest_getopt(3, arguments, "n:", options, NULL) == 'n');
    assert(!strcmp(*guest_optarg, "example") && *guest_optind == 3);
    assert(guest_getopt(3, arguments, "n:", options, NULL) == -1);
    assert(android_libc_compat_resolve("environ", "LIBC", "libc.so", &address));
    extern char **environ;
    char ***guest_environ = (char ***)(uintptr_t)address;
    assert(setenv("MORROW_GLOBAL_TEST", "live", 1) == 0);
    assert(*guest_environ == environ);
    assert(unsetenv("MORROW_GLOBAL_TEST") == 0);
    assert(android_libc_compat_resolve("tzname", "LIBC", "libc.so", &address));
    char **guest_zones = (char **)(uintptr_t)address;
    assert(android_libc_compat_resolve("timezone", "LIBC", "libc.so", &address));
    long *guest_timezone = (long *)(uintptr_t)address;
    assert(android_libc_compat_resolve("daylight", "LIBC", "libc.so", &address));
    int *guest_daylight = (int *)(uintptr_t)address;
    const char *old_zone = getenv("TZ");
    char *saved_zone = old_zone ? strdup(old_zone) : NULL;
    assert(setenv("TZ", "EST5EDT", 1) == 0);
    tzset();
    assert(*guest_timezone == 18000 && *guest_daylight != 0);
    assert(!strcmp(guest_zones[0], "EST") && !strcmp(guest_zones[1], "EDT"));
    assert(setenv("TZ", "UTC0", 1) == 0);
    tzset();
    assert(*guest_timezone == 0 && *guest_daylight == 0 && !strcmp(guest_zones[0], "UTC"));
    if (saved_zone) { assert(setenv("TZ", saved_zone, 1) == 0); free(saved_zone); }
    else assert(unsetenv("TZ") == 0);
    tzset();
    assert(android_stdio_compat_resolve("fdopen", "LIBC", "libc.so", &address));
    FILE *(*guest_fdopen)(int, const char *) = (FILE *(*)(int, const char *))(uintptr_t)address;
    temporary = tmpfile();
    assert(temporary);
    int duplicate = dup(fileno(temporary));
    assert(duplicate >= 0);
    FILE *wrapped = guest_fdopen(duplicate, "w+");
    assert(wrapped && fputs("descriptor", wrapped) >= 0 && fclose(wrapped) == 0);
    rewind(temporary);
    memset(buffer, 0, sizeof(buffer));
    assert(fread(buffer, 1, 10, temporary) == 10 && !strcmp(buffer, "descriptor"));
    assert(fclose(temporary) == 0);

    puts("android_platform_compat_test: Bionic stdio, libc and fortify adapters passed");
    return 0;
}
