#include "android_media_codec_compat.h"

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

static uint64_t symbol(const char *name)
{
    uint64_t address = 0;
    assert(android_media_codec_compat_resolve(name, "LIBMEDIANDK", "libmediandk.so", &address));
    assert(address);
    return address;
}

#define BIND(name, type) ((type)(uintptr_t)symbol(name))

int main(void)
{
    void *(*decoder)(const char *) = BIND("AMediaCodec_createDecoderByType", void *(*)(const char *));
    void *(*encoder)(const char *) = BIND("AMediaCodec_createEncoderByType", void *(*)(const char *));
    assert(decoder("video/avc") == NULL);
    assert(encoder("video/avc") == NULL);
    int32_t (*start)(void *) = BIND("AMediaCodec_start", int32_t (*)(void *));
    int32_t (*stop)(void *) = BIND("AMediaCodec_stop", int32_t (*)(void *));
    int32_t (*flush)(void *) = BIND("AMediaCodec_flush", int32_t (*)(void *));
    int32_t (*remove)(void *) = BIND("AMediaCodec_delete", int32_t (*)(void *));
    assert(start(NULL) == -10003 && stop(NULL) == -10003);
    assert(flush(NULL) == -10003 && remove(NULL) == -10003);
    int32_t (*configure)(void *, const void *, void *, void *, uint32_t) =
        BIND("AMediaCodec_configure", int32_t (*)(void *, const void *, void *, void *, uint32_t));
    assert(configure(NULL, NULL, NULL, NULL, 0) == -10003);
    uint8_t *(*get_input)(void *, size_t, size_t *) =
        BIND("AMediaCodec_getInputBuffer", uint8_t *(*)(void *, size_t, size_t *));
    uint8_t *(*get_output)(void *, size_t, size_t *) =
        BIND("AMediaCodec_getOutputBuffer", uint8_t *(*)(void *, size_t, size_t *));
    size_t size = SIZE_MAX;
    assert(!get_input(NULL, 1, &size) && !size);
    size = SIZE_MAX;
    assert(!get_output(NULL, 1, &size) && !size);
    intptr_t (*dequeue_input)(void *, int64_t) =
        BIND("AMediaCodec_dequeueInputBuffer", intptr_t (*)(void *, int64_t));
    intptr_t (*dequeue_output)(void *, void *, int64_t) =
        BIND("AMediaCodec_dequeueOutputBuffer", intptr_t (*)(void *, void *, int64_t));
    assert(dequeue_input(NULL, 0) == -10003);
    assert(dequeue_output(NULL, NULL, 0) == -10003);
    int32_t (*queue)(void *, size_t, int64_t, size_t, uint64_t, uint32_t) =
        BIND("AMediaCodec_queueInputBuffer", int32_t (*)(void *, size_t, int64_t, size_t, uint64_t, uint32_t));
    assert(queue(NULL, 0, 0, 0, 0, 0) == -10003);
    int32_t (*release)(void *, size_t, bool) =
        BIND("AMediaCodec_releaseOutputBuffer", int32_t (*)(void *, size_t, bool));
    assert(release(NULL, 0, false) == -10003);
    void *(*output_format)(void *) = BIND("AMediaCodec_getOutputFormat", void *(*)(void *));
    assert(output_format(NULL) == NULL);

    uint64_t other = 0;
    assert(!android_media_codec_compat_resolve("AMediaCodec_start", "LIBC", "libc.so", &other));
    assert(!android_media_codec_compat_resolve("AMediaCodec_start", "LIBMEDIANDK", "libc.so", &other));
    assert(!android_media_codec_compat_resolve("AMediaCodec_superpower", NULL, NULL, &other));
    puts("android_media_codec_compat_test: codec unavailable with truthful errors passed");
    return 0;
}
