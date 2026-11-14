#include "android_media_codec_compat.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/types.h>

enum { AMEDIA_ERROR_INVALID_OBJECT = -10003 };

typedef struct UnavailableMediaCodec UnavailableMediaCodec;
typedef struct NativeMediaFormat NativeMediaFormat;
typedef struct {
    int32_t offset;
    int32_t size;
    int64_t presentation_time_us;
    uint32_t flags;
} NativeMediaBufferInfo;

static UnavailableMediaCodec *codec_create(const char *mime)
{
    (void)mime;
    return NULL;
}

static int32_t codec_delete(UnavailableMediaCodec *codec)
{
    (void)codec;
    return AMEDIA_ERROR_INVALID_OBJECT;
}

static int32_t codec_configure(UnavailableMediaCodec *codec,
                               const NativeMediaFormat *format, void *window,
                               void *crypto, uint32_t flags)
{
    (void)codec; (void)format; (void)window; (void)crypto; (void)flags;
    return AMEDIA_ERROR_INVALID_OBJECT;
}

static int32_t codec_state(UnavailableMediaCodec *codec)
{
    (void)codec;
    return AMEDIA_ERROR_INVALID_OBJECT;
}

static uint8_t *codec_get_buffer(UnavailableMediaCodec *codec, size_t index,
                                  size_t *out_size)
{
    (void)codec; (void)index;
    if (out_size) *out_size = 0;
    return NULL;
}

static ssize_t codec_dequeue_input(UnavailableMediaCodec *codec, int64_t timeout_us)
{
    (void)codec; (void)timeout_us;
    return AMEDIA_ERROR_INVALID_OBJECT;
}

static ssize_t codec_dequeue_output(UnavailableMediaCodec *codec,
                                     NativeMediaBufferInfo *info, int64_t timeout_us)
{
    (void)codec; (void)info; (void)timeout_us;
    return AMEDIA_ERROR_INVALID_OBJECT;
}

static int32_t codec_queue_input(UnavailableMediaCodec *codec, size_t index,
                                  int64_t offset, size_t size, uint64_t presentation_time,
                                  uint32_t flags)
{
    (void)codec; (void)index; (void)offset; (void)size;
    (void)presentation_time; (void)flags;
    return AMEDIA_ERROR_INVALID_OBJECT;
}

static int32_t codec_release_output(UnavailableMediaCodec *codec, size_t index,
                                     bool render)
{
    (void)codec; (void)index; (void)render;
    return AMEDIA_ERROR_INVALID_OBJECT;
}

static NativeMediaFormat *codec_output_format(UnavailableMediaCodec *codec)
{
    (void)codec;
    return NULL;
}

bool android_media_codec_compat_resolve(const char *name, const char *version,
                                        const char *library, uint64_t *address)
{
    if (!name || !address ||
        ((version || library) &&
         (!version || !library || strcmp(version, "LIBMEDIANDK") != 0 ||
          strcmp(library, "libmediandk.so") != 0)))
        return false;
    *address = 0;
#define MEDIA_MAP(symbol, implementation) \
    if (strcmp(name, #symbol) == 0) \
        *address = (uint64_t)(uintptr_t)&implementation; else
    MEDIA_MAP(AMediaCodec_createDecoderByType, codec_create)
    MEDIA_MAP(AMediaCodec_createEncoderByType, codec_create)
    MEDIA_MAP(AMediaCodec_delete, codec_delete)
    MEDIA_MAP(AMediaCodec_configure, codec_configure)
    MEDIA_MAP(AMediaCodec_start, codec_state)
    MEDIA_MAP(AMediaCodec_stop, codec_state)
    MEDIA_MAP(AMediaCodec_flush, codec_state)
    MEDIA_MAP(AMediaCodec_getInputBuffer, codec_get_buffer)
    MEDIA_MAP(AMediaCodec_getOutputBuffer, codec_get_buffer)
    MEDIA_MAP(AMediaCodec_dequeueInputBuffer, codec_dequeue_input)
    MEDIA_MAP(AMediaCodec_dequeueOutputBuffer, codec_dequeue_output)
    MEDIA_MAP(AMediaCodec_queueInputBuffer, codec_queue_input)
    MEDIA_MAP(AMediaCodec_getOutputFormat, codec_output_format)
    MEDIA_MAP(AMediaCodec_releaseOutputBuffer, codec_release_output)
    (void)0;
#undef MEDIA_MAP
    return *address != 0;
}
