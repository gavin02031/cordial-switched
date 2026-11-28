#include "android_image_reader_compat.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum {
    AMEDIA_ERROR_UNSUPPORTED = -10002,
    AMEDIA_ERROR_INVALID_OBJECT = -10003
};

// This mirrors Cordial's real (but unavailable) video-decoding boundary.
// Hardware-backed video is not implemented on Horizon; an Android caller
// must see the failure and null output instead of a fake AMEDIA_OK.
static int32_t reader_new_with_usage(int32_t width, int32_t height,
                                      int32_t format, uint64_t usage,
                                      int32_t max_images, void **out_reader)
{
    (void)width; (void)height; (void)format; (void)usage; (void)max_images;
    if (out_reader) *out_reader = NULL;
    return AMEDIA_ERROR_UNSUPPORTED;
}

static void reader_delete(void *reader)
{
    // No constructor here can produce a reader. A non-null pointer could
    // belong to another provider; it must never be freed by the host libc.
    if (reader) fputs("Morrow: unowned Android AImageReader cannot be deleted\n", stderr);
}

static int32_t reader_get_window(void *reader, void **out_window)
{
    (void)reader;
    if (out_window) *out_window = NULL;
    return AMEDIA_ERROR_INVALID_OBJECT;
}

static int32_t reader_acquire_next_image(void *reader, void **out_image)
{
    (void)reader;
    if (out_image) *out_image = NULL;
    return AMEDIA_ERROR_INVALID_OBJECT;
}

static void image_delete(void *image)
{
    if (image) fputs("Morrow: unowned Android AImage cannot be deleted\n", stderr);
}

static int32_t image_get_hardware_buffer(const void *image, void **out_buffer)
{
    (void)image;
    if (out_buffer) *out_buffer = NULL;
    return AMEDIA_ERROR_INVALID_OBJECT;
}

static int32_t image_get_timestamp(const void *image, int64_t *timestamp)
{
    (void)image;
    if (timestamp) *timestamp = 0;
    return AMEDIA_ERROR_INVALID_OBJECT;
}

static void hardware_buffer_acquire(void *buffer)
{
    if (buffer) fputs("Morrow: unowned Android hardware buffer acquire refused\n", stderr);
}

static void hardware_buffer_release(void *buffer)
{
    if (buffer) fputs("Morrow: unowned Android hardware buffer release refused\n", stderr);
}

bool android_image_reader_compat_resolve(const char *name, const char *version,
                                         const char *library, uint64_t *address)
{
    if (!name || !address ||
        ((version || library) &&
         (!version || !library || strcmp(version, "LIBMEDIANDK") != 0 ||
          strcmp(library, "libmediandk.so") != 0)))
        return false;
    *address = 0;
#define IMAGE_MAP(symbol, implementation) \
    if (strcmp(name, #symbol) == 0) \
        *address = (uint64_t)(uintptr_t)&implementation; else
    IMAGE_MAP(AImageReader_newWithUsage, reader_new_with_usage)
    IMAGE_MAP(AImageReader_delete, reader_delete)
    IMAGE_MAP(AImageReader_getWindow, reader_get_window)
    IMAGE_MAP(AImageReader_acquireNextImage, reader_acquire_next_image)
    IMAGE_MAP(AImage_delete, image_delete)
    IMAGE_MAP(AImage_getHardwareBuffer, image_get_hardware_buffer)
    IMAGE_MAP(AImage_getTimestamp, image_get_timestamp)
    IMAGE_MAP(AHardwareBuffer_acquire, hardware_buffer_acquire)
    IMAGE_MAP(AHardwareBuffer_release, hardware_buffer_release)
    (void)0;
#undef IMAGE_MAP
    return *address != 0;
}
