#include "android_fortify_compat.h"

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Signatures and failure semantics are checked against AOSP Bionic's
// libc/bionic/fortify.cpp. A checked operation must stop on an invalid bound;
// allowing the caller to continue after a failed check would conceal an OOB.
static _Noreturn void bad_bound(const char *operation)
{
    fprintf(stderr, "Bionic FORTIFY violation in %s\n", operation);
    abort();
}

static void *fortify_memcpy(void *dest, const void *source, size_t count,
                            size_t dest_len)
{
    if (count > dest_len) bad_bound("memcpy");
    return memcpy(dest, source, count);
}

// Bionic's checked mempcpy returns the byte immediately after the copy.
// Do not calculate that pointer until the destination bound is validated.
static void *fortify_mempcpy(void *dest, const void *source, size_t count,
                             size_t dest_len)
{
    if (count > dest_len) bad_bound("mempcpy");
    memcpy(dest, source, count);
    return (unsigned char *)dest + count;
}

static void *fortify_memmove(void *dest, const void *source, size_t count,
                             size_t dest_len)
{
    if (count > dest_len) bad_bound("memmove");
    return memmove(dest, source, count);
}

static void *fortify_memset(void *dest, int byte, size_t count, size_t dest_len)
{
    if (count > dest_len) bad_bound("memset");
    return memset(dest, byte, count);
}

static size_t fortify_strlen(const char *text, size_t source_len)
{
    // Unlike strlen followed by a length check, this will not inspect beyond
    // the buffer size supplied by the Android compiler's fortify pass.
    for (size_t index = 0; index < source_len; index++)
        if (text[index] == 0)
            return index;
    bad_bound("strlen");
}

static char *fortify_strcpy(char *dest, const char *source, size_t dest_len)
{
    const size_t len = fortify_strlen(source, dest_len) + 1;
    return memcpy(dest, source, len);
}

static char *fortify_strncpy(char *dest, const char *source, size_t count,
                             size_t dest_len)
{
    if (count > dest_len) bad_bound("strncpy");
    return strncpy(dest, source, count);
}

static char *fortify_strncpy2(char *dest, const char *source, size_t count,
                              size_t dest_len, size_t source_len)
{
    if (count > dest_len) bad_bound("strncpy destination");
    size_t i = 0;
    for (; i < count; i++) {
        if (i >= source_len) bad_bound("strncpy source");
        dest[i] = source[i];
        if (!source[i]) {
            memset(dest + i + 1, 0, count - i - 1);
            return dest;
        }
    }
    return dest;
}

static int *fortify_errno(void)
{
    // The Switch C runtime owns errno storage. Returning its per-thread
    // address keeps the Android caller consistent with our native wrappers.
    return &errno;
}

static _Noreturn void fortify_assert(const char *file, int line,
                                     const char *message)
{
    fprintf(stderr, "Android assertion at %s:%d: %s\n",
            file ? file : "(unknown)", line,
            message ? message : "(unknown)");
    abort();
}

bool android_fortify_compat_resolve(const char *name, const char *version,
                                    const char *library, uint64_t *address)
{
    if (!name || !version || !library || !address ||
        strcmp(library, "libc.so") != 0)
        return false;
    *address = 0;
    if (strcmp(version, "LIBC") != 0)
        return false;

#define MAP_FORTIFY(symbol, target) \
    if (strcmp(name, symbol) == 0) *address = (uint64_t)(uintptr_t)(target); else
    MAP_FORTIFY("__memcpy_chk", &fortify_memcpy)
    MAP_FORTIFY("__mempcpy_chk", &fortify_mempcpy)
    MAP_FORTIFY("__memmove_chk", &fortify_memmove)
    MAP_FORTIFY("__memset_chk", &fortify_memset)
    MAP_FORTIFY("__strlen_chk", &fortify_strlen)
    MAP_FORTIFY("__strcpy_chk", &fortify_strcpy)
    MAP_FORTIFY("__strncpy_chk", &fortify_strncpy)
    MAP_FORTIFY("__strncpy_chk2", &fortify_strncpy2)
    MAP_FORTIFY("__errno", &fortify_errno)
    MAP_FORTIFY("__assert", &fortify_assert)
    *address = 0;
#undef MAP_FORTIFY
    return *address != 0;
}
