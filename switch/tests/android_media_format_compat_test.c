#include "android_media_format_compat.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static uint64_t symbol(const char *name)
{
    uint64_t address = 0;
    assert(android_media_format_compat_resolve(name, "LIBMEDIANDK", "libmediandk.so", &address));
    assert(address);
    return address;
}

#define BIND(name, type) ((type)(uintptr_t)symbol(name))

int main(void)
{
    void *(*format_new)(void) = BIND("AMediaFormat_new", void *(*)(void));
    int32_t (*format_delete)(void *) = BIND("AMediaFormat_delete", int32_t (*)(void *));
    void (*set_int32)(void *, const char *, int32_t) =
        BIND("AMediaFormat_setInt32", void (*)(void *, const char *, int32_t));
    bool (*get_int32)(void *, const char *, int32_t *) =
        BIND("AMediaFormat_getInt32", bool (*)(void *, const char *, int32_t *));
    void (*set_float)(void *, const char *, float) =
        BIND("AMediaFormat_setFloat", void (*)(void *, const char *, float));
    void (*set_string)(void *, const char *, const char *) =
        BIND("AMediaFormat_setString", void (*)(void *, const char *, const char *));
    void (*set_buffer)(void *, const char *, const void *, size_t) =
        BIND("AMediaFormat_setBuffer", void (*)(void *, const char *, const void *, size_t));
    bool (*get_buffer)(void *, const char *, void **, size_t *) =
        BIND("AMediaFormat_getBuffer", bool (*)(void *, const char *, void **, size_t *));
    const char *(*format_string)(void *) =
        BIND("AMediaFormat_toString", const char *(*)(void *));

    const char *const *mime_key = (const char *const *)(uintptr_t)symbol("AMEDIAFORMAT_KEY_MIME");
    const char *const *rate_key = (const char *const *)(uintptr_t)symbol("AMEDIAFORMAT_KEY_BIT_RATE");
    assert(strcmp(*mime_key, "mime") == 0);
    assert(strcmp(*rate_key, "bitrate") == 0);

    void *format = format_new();
    assert(format);
    int32_t rate = 0;
    assert(!get_int32(format, *rate_key, &rate));
    set_int32(format, *rate_key, 192000);
    assert(get_int32(format, *rate_key, &rate) && rate == 192000);
    set_float(format, "frame-rate", 59.94f);
    assert(!get_int32(format, "frame-rate", &rate));
    char mime[] = "video/mp4";
    set_string(format, *mime_key, mime);
    mime[0] = 'X';
    assert(strstr(format_string(format), "mime") != NULL);
    unsigned char source[4] = { 1, 2, 3, 4 };
    set_buffer(format, "csd-0", source, sizeof(source));
    source[0] = 99;
    void *buffer = NULL;
    size_t size = 0;
    assert(get_buffer(format, "csd-0", &buffer, &size));
    assert(size == 4 && ((unsigned char *)buffer)[0] == 1);
    set_int32(format, "csd-0", 3);
    assert(!get_buffer(format, "csd-0", &buffer, &size));
    assert(get_int32(format, "csd-0", &rate) && rate == 3);
    // A fully populated format must still leave room for the string's
    // closing brace and NUL when the debug representation is truncated.
    for (unsigned i = 0; i < 120; i++) {
        char key[112];
        snprintf(key, sizeof(key), "key-%03u-padding-to-exercise-the-fixed-debug-buffer", i);
        set_int32(format, key, (int32_t)i);
    }
    const char *printed = format_string(format);
    assert(printed && printed[0] == '{');
    assert(printed[strlen(printed) - 1] == '}');
    assert(format_delete(format) == 0);
    assert(format_delete(NULL) == 0);

    uint64_t address = 123;
    assert(!android_media_format_compat_resolve("AMediaFormat_delete", "LIBC",
                                                 "libmediandk.so", &address));
    assert(!android_media_format_compat_resolve("AMediaFormat_delete", "LIBMEDIANDK",
                                                 "libc.so", &address));
    assert(!android_media_format_compat_resolve("AMediaCodec_start", "LIBMEDIANDK",
                                                 "libmediandk.so", &address));
    assert(!android_media_format_compat_resolve("made_up_success", NULL, NULL, &address));
    assert(android_media_format_compat_resolve("AMediaFormat_delete", NULL, NULL, &address));
    puts("android_media_format_compat_test: ownership, key data and versioned symbols passed");
    return 0;
}
