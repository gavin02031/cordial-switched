#include "android_image_reader_compat.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

static uint64_t symbol(const char *name)
{
    uint64_t address = 0;
    assert(android_image_reader_compat_resolve(name, NULL, NULL, &address));
    assert(address);
    return address;
}

#define BIND(name, type) ((type)(uintptr_t)symbol(name))

int main(void)
{
    int32_t (*construct)(int32_t, int32_t, int32_t, uint64_t, int32_t, void **) =
        BIND("AImageReader_newWithUsage",
             int32_t (*)(int32_t, int32_t, int32_t, uint64_t, int32_t, void **));
    void *reader = (void *)(uintptr_t)1;
    assert(construct(1280, 720, 35, 0x100, 4, &reader) == -10002);
    assert(reader == NULL);
    assert(construct(0, 0, 0, 0, 0, NULL) == -10002);

    void (*delete_reader)(void *) = BIND("AImageReader_delete", void (*)(void *));
    delete_reader(reader);
    void *window = (void *)(uintptr_t)1;
    int32_t (*get_window)(void *, void **) =
        BIND("AImageReader_getWindow", int32_t (*)(void *, void **));
    assert(get_window(NULL, &window) == -10003 && !window);
    void *image = (void *)(uintptr_t)1;
    int32_t (*acquire)(void *, void **) =
        BIND("AImageReader_acquireNextImage", int32_t (*)(void *, void **));
    assert(acquire(NULL, &image) == -10003 && !image);
    void (*delete_image)(void *) = BIND("AImage_delete", void (*)(void *));
    delete_image(NULL);
    void *buffer = (void *)(uintptr_t)1;
    int32_t (*get_buffer)(const void *, void **) =
        BIND("AImage_getHardwareBuffer", int32_t (*)(const void *, void **));
    assert(get_buffer(NULL, &buffer) == -10003 && !buffer);
    int64_t timestamp = 123;
    int32_t (*get_timestamp)(const void *, int64_t *) =
        BIND("AImage_getTimestamp", int32_t (*)(const void *, int64_t *));
    assert(get_timestamp(NULL, &timestamp) == -10003 && !timestamp);
    void (*buffer_acquire)(void *) = BIND("AHardwareBuffer_acquire", void (*)(void *));
    void (*buffer_release)(void *) = BIND("AHardwareBuffer_release", void (*)(void *));
    buffer_acquire(NULL);
    buffer_release(NULL);
    uint64_t versioned = 0;
    assert(android_image_reader_compat_resolve("AImageReader_delete", "LIBMEDIANDK",
                                                "libmediandk.so", &versioned));
    assert(versioned == (uint64_t)(uintptr_t)delete_reader);
    assert(!android_image_reader_compat_resolve("AImageReader_delete", "LIBC",
                                                 "libc.so", &versioned));
    assert(!android_image_reader_compat_resolve("AImageReader_fake", NULL,
                                                 NULL, &versioned));
    assert(!android_image_reader_compat_resolve("AImageReader_delete", NULL,
                                                 NULL, NULL));
    puts("android_image_reader_compat_test: honest unsupported media path passed");
    return 0;
}
