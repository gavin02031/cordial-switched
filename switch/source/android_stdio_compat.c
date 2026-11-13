#define _POSIX_C_SOURCE 200809L
#include "android_stdio_compat.h"

#include <errno.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>

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

static FILE *host_stream(FILE *stream)
{
    if (stream == android_stdin_data) return stdin;
    if (stream == android_stdout_data) return stdout;
    if (stream == android_stderr_data) return stderr;
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

static size_t stdio_fread(void *data, size_t size, size_t count, FILE *stream)
{
    return fread(data, size, count, host_stream(stream));
}

static size_t stdio_fwrite(const void *data, size_t size, size_t count, FILE *stream)
{
    return fwrite(data, size, count, host_stream(stream));
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

static int stdio_fileno(FILE *stream)
{
    return fileno(host_stream(stream));
}

static int stdio_fclose(FILE *stream)
{
    // Keep the host's process-global streams alive if an Android caller
    // tries to close one. The caller receives an actual failure.
    if (stream == android_stdin_data || stream == android_stdout_data ||
        stream == android_stderr_data) {
        errno = EBADF;
        return EOF;
    }
    return fclose(stream);
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
        strcmp(version, "LIBC") != 0 || strcmp(library, "libc.so") != 0)
        return false;
    *address = 0;
    // Do not advertise __sF: our handles are not a real contiguous Bionic
    // FILE array, and Android code indexing them would corrupt memory.
#define MAP_STDIO(symbol, target) \
    if (strcmp(name, symbol) == 0) *address = (uint64_t)(uintptr_t)(target); else
    MAP_STDIO("stderr", &android_stderr_data)
    MAP_STDIO("stdout", &android_stdout_data)
    MAP_STDIO("stdin", &android_stdin_data)
    MAP_STDIO("fprintf", &stdio_fprintf)
    MAP_STDIO("vfprintf", &stdio_vfprintf)
    MAP_STDIO("fread", &stdio_fread)
    MAP_STDIO("fwrite", &stdio_fwrite)
    MAP_STDIO("fputs", &stdio_fputs)
    MAP_STDIO("fputc", &stdio_fputc)
    MAP_STDIO("fflush", &stdio_fflush)
    MAP_STDIO("feof", &stdio_feof)
    MAP_STDIO("fileno", &stdio_fileno)
    MAP_STDIO("fclose", &stdio_fclose)
    MAP_STDIO("fopen", &fopen)
    MAP_STDIO("fseeko", &stdio_fseeko)
    MAP_STDIO("ftello", &stdio_ftello)
    *address = 0;
#undef MAP_STDIO
    return *address != 0;
}
