#include "android_media_format_compat.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { MAX_FORMAT_ENTRIES = 128, MAX_FORMAT_KEY_BYTES = 128,
       MAX_FORMAT_BUFFER_BYTES = 4 * 1024 * 1024 };

typedef enum { MEDIA_INT32, MEDIA_FLOAT, MEDIA_STRING, MEDIA_BUFFER } MediaType;

typedef struct {
    char *key;
    MediaType type;
    union {
        int32_t integer;
        float floating;
        char *string;
        struct { unsigned char *bytes; size_t length; } buffer;
    } value;
} MediaEntry;

typedef struct {
    MediaEntry entries[MAX_FORMAT_ENTRIES];
    size_t count;
    char printable[1024];
} NativeMediaFormat;

// Exported data symbols are pointers to immutable NDK keys, not the bytes of
// the keys themselves. Their addresses are used in AArch64 GLOB_DAT records.
static const char *media_key_bit_rate = "bitrate";
static const char *media_key_channel_count = "channel-count";
static const char *media_key_color_format = "color-format";
static const char *media_key_frame_rate = "frame-rate";
static const char *media_key_height = "height";
static const char *media_key_i_frame_interval = "i-frame-interval";
static const char *media_key_mime = "mime";
static const char *media_key_sample_rate = "sample-rate";
static const char *media_key_stride = "stride";
static const char *media_key_width = "width";

static char *copy_text(const char *value, size_t length)
{
    if (length == SIZE_MAX) return NULL;
    char *copy = malloc(length + 1);
    if (!copy) return NULL;
    memcpy(copy, value, length);
    copy[length] = 0;
    return copy;
}

static MediaEntry *find_entry(NativeMediaFormat *format, const char *key)
{
    if (!format || !key) return NULL;
    for (size_t i = 0; i < format->count; i++)
        if (strcmp(format->entries[i].key, key) == 0)
            return &format->entries[i];
    return NULL;
}

static MediaEntry *ensure_entry(NativeMediaFormat *format, const char *key)
{
    if (!format || !key) return NULL;
    MediaEntry *existing = find_entry(format, key);
    if (existing) return existing;
    const size_t length = strlen(key);
    if (!length || length > MAX_FORMAT_KEY_BYTES ||
        format->count >= MAX_FORMAT_ENTRIES)
        return NULL;
    char *copy = copy_text(key, length);
    if (!copy) return NULL;
    MediaEntry *entry = &format->entries[format->count++];
    memset(entry, 0, sizeof(*entry));
    entry->key = copy;
    return entry;
}

static void clear_value(MediaEntry *entry)
{
    if (entry->type == MEDIA_STRING) free(entry->value.string);
    if (entry->type == MEDIA_BUFFER) free(entry->value.buffer.bytes);
    memset(&entry->value, 0, sizeof(entry->value));
}

static NativeMediaFormat *media_format_new(void)
{
    return calloc(1, sizeof(NativeMediaFormat));
}

static int32_t media_format_delete(NativeMediaFormat *format)
{
    if (!format) return 0;
    for (size_t i = 0; i < format->count; i++) {
        clear_value(&format->entries[i]);
        free(format->entries[i].key);
    }
    free(format);
    return 0; // AMEDIA_OK; a destructor with no codec side effects.
}

static void media_format_set_int32(NativeMediaFormat *format,
                                   const char *key, int32_t value)
{
    MediaEntry *entry = ensure_entry(format, key);
    if (!entry) return;
    clear_value(entry);
    entry->type = MEDIA_INT32;
    entry->value.integer = value;
}

static bool media_format_get_int32(NativeMediaFormat *format,
                                   const char *key, int32_t *out)
{
    MediaEntry *entry = find_entry(format, key);
    if (!entry || !out || entry->type != MEDIA_INT32) return false;
    *out = entry->value.integer;
    return true;
}

static void media_format_set_float(NativeMediaFormat *format,
                                   const char *key, float value)
{
    MediaEntry *entry = ensure_entry(format, key);
    if (!entry) return;
    clear_value(entry);
    entry->type = MEDIA_FLOAT;
    entry->value.floating = value;
}

static void media_format_set_string(NativeMediaFormat *format,
                                    const char *key, const char *value)
{
    if (!value) return;
    char *copy = copy_text(value, strlen(value));
    if (!copy) return;
    MediaEntry *entry = ensure_entry(format, key);
    if (!entry) { free(copy); return; }
    clear_value(entry);
    entry->type = MEDIA_STRING;
    entry->value.string = copy;
}

static void media_format_set_buffer(NativeMediaFormat *format,
                                    const char *key, const void *data, size_t bytes)
{
    if ((!data && bytes) || bytes > MAX_FORMAT_BUFFER_BYTES) return;
    unsigned char *copy = malloc(bytes ? bytes : 1);
    if (!copy) return;
    if (bytes) memcpy(copy, data, bytes);
    MediaEntry *entry = ensure_entry(format, key);
    if (!entry) { free(copy); return; }
    clear_value(entry);
    entry->type = MEDIA_BUFFER;
    entry->value.buffer.bytes = copy;
    entry->value.buffer.length = bytes;
}

static bool media_format_get_buffer(NativeMediaFormat *format, const char *key,
                                    void **out_data, size_t *out_size)
{
    MediaEntry *entry = find_entry(format, key);
    if (!entry || !out_data || !out_size || entry->type != MEDIA_BUFFER)
        return false;
    *out_data = entry->value.buffer.bytes;
    *out_size = entry->value.buffer.length;
    return true;
}

static const char *media_format_to_string(NativeMediaFormat *format)
{
    if (!format) return NULL;
    size_t used = 0;
    format->printable[used++] = '{';
    for (size_t i = 0; i < format->count; i++) {
        const MediaEntry *entry = &format->entries[i];
        char value[80];
        if (entry->type == MEDIA_INT32)
            snprintf(value, sizeof(value), "int32(%ld)", (long)entry->value.integer);
        else if (entry->type == MEDIA_FLOAT)
            snprintf(value, sizeof(value), "float(%g)", (double)entry->value.floating);
        else if (entry->type == MEDIA_STRING)
            snprintf(value, sizeof(value), "string");
        else
            snprintf(value, sizeof(value), "buffer(%zu)", entry->value.buffer.length);
        // Reserve the final brace and NUL even when the entry is truncated.
        const size_t available = sizeof(format->printable) - used - 2;
        const int written = snprintf(format->printable + used,
                                     available + 1,
                                     "%s%.128s: %s", i ? ", " : "", entry->key, value);
        if (written < 0) break;
        if ((size_t)written > available) {
            used = sizeof(format->printable) - 2;
            break;
        }
        used += (size_t)written;
    }
    format->printable[used++] = '}';
    format->printable[used] = 0;
    return format->printable;
}

bool android_media_format_compat_resolve(const char *name, const char *version,
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
    MEDIA_MAP(AMediaFormat_new, media_format_new)
    MEDIA_MAP(AMediaFormat_delete, media_format_delete)
    MEDIA_MAP(AMediaFormat_toString, media_format_to_string)
    MEDIA_MAP(AMediaFormat_getInt32, media_format_get_int32)
    MEDIA_MAP(AMediaFormat_getBuffer, media_format_get_buffer)
    MEDIA_MAP(AMediaFormat_setInt32, media_format_set_int32)
    MEDIA_MAP(AMediaFormat_setFloat, media_format_set_float)
    MEDIA_MAP(AMediaFormat_setString, media_format_set_string)
    MEDIA_MAP(AMediaFormat_setBuffer, media_format_set_buffer)
    MEDIA_MAP(AMEDIAFORMAT_KEY_BIT_RATE, media_key_bit_rate)
    MEDIA_MAP(AMEDIAFORMAT_KEY_CHANNEL_COUNT, media_key_channel_count)
    MEDIA_MAP(AMEDIAFORMAT_KEY_COLOR_FORMAT, media_key_color_format)
    MEDIA_MAP(AMEDIAFORMAT_KEY_FRAME_RATE, media_key_frame_rate)
    MEDIA_MAP(AMEDIAFORMAT_KEY_HEIGHT, media_key_height)
    MEDIA_MAP(AMEDIAFORMAT_KEY_I_FRAME_INTERVAL, media_key_i_frame_interval)
    MEDIA_MAP(AMEDIAFORMAT_KEY_MIME, media_key_mime)
    MEDIA_MAP(AMEDIAFORMAT_KEY_SAMPLE_RATE, media_key_sample_rate)
    MEDIA_MAP(AMEDIAFORMAT_KEY_STRIDE, media_key_stride)
    MEDIA_MAP(AMEDIAFORMAT_KEY_WIDTH, media_key_width)
    (void)0;
#undef MEDIA_MAP
    return *address != 0;
}
