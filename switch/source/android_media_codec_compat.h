#ifndef CORDIAL_ANDROID_MEDIA_CODEC_COMPAT_H
#define CORDIAL_ANDROID_MEDIA_CODEC_COMPAT_H

#include <stdbool.h>
#include <stdint.h>

// Decoder/encoder discovery reports unavailable until a real Switch media
// backend exists. Every method requiring a codec returns an explicit error,
// never success or a fabricated codec pointer.
bool android_media_codec_compat_resolve(const char *name, const char *version,
                                        const char *library, uint64_t *address);

#endif
