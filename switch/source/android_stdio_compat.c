#define _POSIX_C_SOURCE 200809L
#include "android_stdio_compat.h"

#include <errno.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <wchar.h>

// Android's FILE layout is not newlib's FILE layout. These distinct opaque
// handles are passed only through our wrappers; they must never reach newlib
// unchanged. The explicit indirection is required because Bionic's stderr
// ELF data symbol contains a FILE * rather than being a FILE itself.
// Inspired by the fake __sF and stdio adapters in NaGaa95/angrybirds2_nx.
static _Alignas(16) unsigned char android_stdin_marker[256];
static _Alignas(16) unsigned char android_stdout_marker[256];
static _Alignas(16) unsigned char android_stderr_marker[256];
static FILE *android_stdin_data = (FILE *)android_stdin_marker;
static FILE *android_stdout_data = (FILE *)android_stdout_marker;
static FILE *android_stderr_data = (FILE *)android_stderr_marker;

// A legacy Android dependency still imports the pre-M `FILE __sF[3]` array.
// Bionic's LP64 FILE is an opaque, 152-byte structure, unlike newlib's FILE.
// Keep a correctly strided, readable array, but redirect every FILE-taking
// entry point we advertise to the real host stream. This follows Cordial's
// native/legacy_stdio.cpp, which fixed the _IO_fflush crash on desktop.
// See Android bionic libc/include/bits/struct_file.h for the LP64 stride.
enum { ANDROID_LEGACY_FILE_BYTES = 152, ANDROID_LEGACY_STREAMS = 3 };
static _Alignas(sizeof(void *)) unsigned char
    android_legacy_sf[ANDROID_LEGACY_STREAMS][ANDROID_LEGACY_FILE_BYTES];

static int legacy_stream_index(FILE *stream)
{
    const uintptr_t start = (uintptr_t)&android_legacy_sf[0][0];
    const uintptr_t value = (uintptr_t)stream;
    const size_t size = sizeof(android_legacy_sf);
    if (value < start || value - start >= size) return -1;
    return (int)((value - start) / ANDROID_LEGACY_FILE_BYTES);
}

static bool is_standard_stream(FILE *stream)
{
    return stream == android_stdin_data || stream == android_stdout_data ||
           stream == android_stderr_data || legacy_stream_index(stream) >= 0;
}

static FILE *host_stream(FILE *stream)
{
    if (stream == android_stdin_data) return stdin;
    if (stream == android_stdout_data) return stdout;
    if (stream == android_stderr_data) return stderr;
    switch (legacy_stream_index(stream)) {
    case 0: return stdin;
    case 1: return stdout;
    case 2: return stderr;
    }
    return stream;
}

static int stdio_fprintf(FILE *stream, const char *format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    int result = vfprintf(host_stream(stream), format, arguments);
    va_end(arguments);
    return result;
}

static int stdio_vfprintf(FILE *stream, const char *format, va_list arguments)
{
    return vfprintf(host_stream(stream), format, arguments);
}

static int stdio_vasprintf(char **output, const char *format, va_list arguments)
{
    if (!output || !format) { errno = EINVAL; return -1; }
    *output = NULL;
    va_list measure;
    va_copy(measure, arguments);
    const int length = vsnprintf(NULL, 0, format, measure);
    va_end(measure);
    if (length < 0) return -1;
    char *buffer = malloc((size_t)length + 1);
    if (!buffer) { errno = ENOMEM; return -1; }
    va_list emit;
    va_copy(emit, arguments);
    const int written = vsnprintf(buffer, (size_t)length + 1, format, emit);
    va_end(emit);
    if (written != length) {
        free(buffer);
        errno = EIO;
        return -1;
    }
    *output = buffer;
    return length;
}

static size_t stdio_fread(void *data, size_t size, size_t count, FILE *stream)
{
    return fread(data, size, count, host_stream(stream));
}

static size_t stdio_fwrite(const void *data, size_t size, size_t count, FILE *stream)
{
    return fwrite(data, size, count, host_stream(stream));
}

static size_t stdio_fread_chk(void *data, size_t size, size_t count,
                              FILE *stream, size_t buffer_size)
{
    if (size && (count > SIZE_MAX / size || count * size > buffer_size)) {
        fprintf(stderr, "Bionic FORTIFY violation in fread\n");
        abort();
    }
    return stdio_fread(data, size, count, stream);
}

static size_t stdio_fwrite_chk(const void *data, size_t size, size_t count,
                               FILE *stream, size_t buffer_size)
{
    if (size && (count > SIZE_MAX / size || count * size > buffer_size)) {
        fprintf(stderr, "Bionic FORTIFY violation in fwrite\n");
        abort();
    }
    return stdio_fwrite(data, size, count, stream);
}

static int stdio_fputs(const char *text, FILE *stream)
{
    return fputs(text, host_stream(stream));
}

static int stdio_fputc(int character, FILE *stream)
{
    return fputc(character, host_stream(stream));
}

static int stdio_fflush(FILE *stream)
{
    return fflush(stream ? host_stream(stream) : NULL);
}

static int stdio_feof(FILE *stream)
{
    return feof(host_stream(stream));
}

static int stdio_ferror(FILE *stream)
{
    return ferror(host_stream(stream));
}

static void stdio_clearerr(FILE *stream)
{
    clearerr(host_stream(stream));
}

static char *stdio_fgets(char *buffer, int capacity, FILE *stream)
{
    return fgets(buffer, capacity, host_stream(stream));
}

static int stdio_getc(FILE *stream)
{
    return getc(host_stream(stream));
}

static int stdio_ungetc(int character, FILE *stream)
{
    return ungetc(character, host_stream(stream));
}

static wint_t stdio_getwc(FILE *stream)
{
    return getwc(host_stream(stream));
}

static wint_t stdio_ungetwc(wint_t character, FILE *stream)
{
    return ungetwc(character, host_stream(stream));
}

static wint_t stdio_fputwc(wchar_t character, FILE *stream)
{
    return fputwc(character, host_stream(stream));
}

static int stdio_fscanf(FILE *stream, const char *format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    int result = vfscanf(host_stream(stream), format, arguments);
    va_end(arguments);
    return result;
}

static int stdio_fileno(FILE *stream)
{
    return fileno(host_stream(stream));
}

static int stdio_fclose(FILE *stream)
{
    // Keep the host's process-global streams alive if an Android caller
    // tries to close one. The caller receives an actual failure.
    if (is_standard_stream(stream)) {
        errno = EBADF;
        return EOF;
    }
    return fclose(stream);
}

static int stdio_fseek(FILE *stream, long offset, int whence)
{
    return fseek(host_stream(stream), offset, whence);
}

static long stdio_ftell(FILE *stream)
{
    return ftell(host_stream(stream));
}

static int stdio_setvbuf(FILE *stream, char *buffer, int mode, size_t size)
{
    return setvbuf(host_stream(stream), buffer, mode, size);
}

static int stdio_fseeko(FILE *stream, off_t offset, int whence)
{
    return fseeko(host_stream(stream), offset, whence);
}

static off_t stdio_ftello(FILE *stream)
{
    return ftello(host_stream(stream));
}

bool android_stdio_compat_resolve(const char *name, const char *version,
                                  const char *library, uint64_t *address)
{
    if (!name || !version || !library || !address ||
        strcmp(library, "libc.so") != 0)
        return false;
    *address = 0;
    // Bionic exports fortify-checked fread/fwrite under LIBC_N (API 24+),
    // not under the base LIBC symbol version of their ordinary variants.
    if (strcmp(version, "LIBC_N") == 0) {
        if (strcmp(name, "__fread_chk") == 0)
            *address = (uint64_t)(uintptr_t)&stdio_fread_chk;
        else if (strcmp(name, "__fwrite_chk") == 0)
            *address = (uint64_t)(uintptr_t)&stdio_fwrite_chk;
        return *address != 0;
    }
    if (strcmp(version, "LIBC") != 0)
        return false;
    // The __sF array has the exact Bionic LP64 stride, while its pointers
    // are translated above. Never pass any of its slots to newlib directly.
#define MAP_STDIO(symbol, target) \
    if (strcmp(name, symbol) == 0) *address = (uint64_t)(uintptr_t)(target); else
    MAP_STDIO("__sF", android_legacy_sf)
    MAP_STDIO("stderr", &android_stderr_data)
    MAP_STDIO("stdout", &android_stdout_data)
    MAP_STDIO("stdin", &android_stdin_data)
    MAP_STDIO("fprintf", &stdio_fprintf)
    MAP_STDIO("vfprintf", &stdio_vfprintf)
    MAP_STDIO("vasprintf", &stdio_vasprintf)
    MAP_STDIO("fread", &stdio_fread)
    MAP_STDIO("fwrite", &stdio_fwrite)
    MAP_STDIO("fputs", &stdio_fputs)
    MAP_STDIO("fputc", &stdio_fputc)
    MAP_STDIO("fflush", &stdio_fflush)
    MAP_STDIO("feof", &stdio_feof)
    MAP_STDIO("ferror", &stdio_ferror)
    MAP_STDIO("clearerr", &stdio_clearerr)
    MAP_STDIO("fgets", &stdio_fgets)
    MAP_STDIO("getc", &stdio_getc)
    MAP_STDIO("ungetc", &stdio_ungetc)
    MAP_STDIO("getwc", &stdio_getwc)
    MAP_STDIO("ungetwc", &stdio_ungetwc)
    MAP_STDIO("fputwc", &stdio_fputwc)
    MAP_STDIO("fscanf", &stdio_fscanf)
    MAP_STDIO("fileno", &stdio_fileno)
    MAP_STDIO("fclose", &stdio_fclose)
    MAP_STDIO("fopen", &fopen)
    MAP_STDIO("fdopen", &fdopen)
    MAP_STDIO("fseek", &stdio_fseek)
    MAP_STDIO("ftell", &stdio_ftell)
    MAP_STDIO("setvbuf", &stdio_setvbuf)
    MAP_STDIO("fseeko", &stdio_fseeko)
    MAP_STDIO("ftello", &stdio_ftello)
    *address = 0;
#undef MAP_STDIO
    return *address != 0;
}
