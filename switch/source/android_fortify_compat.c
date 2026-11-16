#define _POSIX_C_SOURCE 200809L
#include "android_fortify_compat.h"

#include <errno.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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
    // The bound belongs to the destination, not the source. Scanning the
    // source for dest_len bytes could read beyond a short source object.
    // Include the NUL byte in the destination check without overflowing.
    const size_t len = strlen(source);
    if (len >= dest_len) bad_bound("strcpy");
    return memcpy(dest, source, len + 1);
}

// A destination without a terminator is not a string, even if the source fits.
// Check the existing destination before considering the appended bytes.
static char *fortify_strcat(char *dest, const char *source, size_t dest_len)
{
    const size_t used = fortify_strlen(dest, dest_len);
    const size_t added = strlen(source);
    if (added >= dest_len - used) bad_bound("strcat");
    memcpy(dest + used, source, added + 1);
    return dest;
}


// Bionic's checked strncat appends at most count source bytes and always
// terminates the destination. Check the destination before reading or writing
// past its advertised capacity; avoid count + 1 overflow.
static char *fortify_strncat(char *dest, const char *source, size_t count,
                             size_t dest_len)
{
    const size_t used = fortify_strlen(dest, dest_len);
    const size_t available = dest_len - used - 1;
    size_t added = 0;
    while (added < count && added <= available && source[added])
        added++;
    if (added > available) bad_bound("strncat");
    memcpy(dest + used, source, added);
    dest[used + added] = 0;
    return dest;
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

static char *fortify_strchr(const char *source, int character, size_t source_len)
{
    // The limit belongs to the source object. An un-terminated string may
    // not be scanned into the next allocation looking for a character.
    for (size_t index = 0; index < source_len; index++) {
        if (source[index] == character)
            return (char *)(source + index);
        if (source[index] == 0)
            return NULL;
    }
    bad_bound("strchr");
}

// Bionic verifies both the object bound and the signed ssize_t result range.
// Do the checks before entering the host fd layer, including on zero-byte I/O.
static ssize_t fortify_read(int fd, void *buffer, size_t count, size_t capacity)
{
    if (count > capacity || count > (size_t)PTRDIFF_MAX) bad_bound("read");
    return read(fd, buffer, count);
}

static ssize_t fortify_write(int fd, const void *buffer, size_t count,
                             size_t capacity)
{
    if (count > capacity || count > (size_t)PTRDIFF_MAX) bad_bound("write");
    return write(fd, buffer, count);
}

static ssize_t fortify_readlink(const char *path, char *buffer, size_t count,
                                size_t capacity)
{
    if (count > capacity || count > (size_t)PTRDIFF_MAX) bad_bound("readlink");
    return readlink(path, buffer, count);
}

static int fortify_vsnprintf(char *dest, size_t count, int flags,
                             size_t capacity, const char *format, va_list args)
{
    (void)flags;
    if (count > capacity) bad_bound("vsnprintf");
    return vsnprintf(dest, count, format, args);
}

static int fortify_vsprintf(char *dest, int flags, size_t capacity,
                            const char *format, va_list args)
{
    (void)flags;
    // Bionic permits SIZE_MAX as an unknown bound, but that provides no
    // meaningful protection in a cross-runtime bridge. Do not convert it
    // into an effectively unbounded write to native memory.
    if (capacity == SIZE_MAX || capacity == 0) bad_bound("vsprintf capacity");
    int result = vsnprintf(dest, capacity, format, args);
    if (result >= 0 && (size_t)result >= capacity) bad_bound("vsprintf");
    return result;
}

static _Noreturn void fortify_stack_chk_fail(void)
{
    bad_bound("stack canary");
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
    // Bionic exposes __write_chk at LIBC_N, unlike __read_chk. Binding the
    // right entrypoint under the wrong symbol version defeats the audit.
    if (strcmp(version, "LIBC_N") == 0 &&
        strcmp(name, "__write_chk") == 0) {
        *address = (uint64_t)(uintptr_t)&fortify_write;
        return true;
    }
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
    MAP_FORTIFY("__strcat_chk", &fortify_strcat)
    MAP_FORTIFY("__strncat_chk", &fortify_strncat)
    MAP_FORTIFY("__strncpy_chk", &fortify_strncpy)
    MAP_FORTIFY("__strncpy_chk2", &fortify_strncpy2)
    MAP_FORTIFY("__strchr_chk", &fortify_strchr)
    MAP_FORTIFY("__read_chk", &fortify_read)
    MAP_FORTIFY("__readlink_chk", &fortify_readlink)
    MAP_FORTIFY("__vsnprintf_chk", &fortify_vsnprintf)
    MAP_FORTIFY("__vsprintf_chk", &fortify_vsprintf)
    MAP_FORTIFY("__stack_chk_fail", &fortify_stack_chk_fail)
    MAP_FORTIFY("__errno", &fortify_errno)
    MAP_FORTIFY("__assert", &fortify_assert)
    *address = 0;
#undef MAP_FORTIFY
    return *address != 0;
}
