#include "engine_tls_scratch.h"

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum { MAX_TLS_TEMPLATE_BYTES = 1024 * 1024 };

void engine_tls_scratch_release(EngineTlsScratch *scratch)
{
    if (!scratch)
        return;
    free(scratch->allocation);
    memset(scratch, 0, sizeof(*scratch));
}

EngineTlsScratchStatus engine_tls_scratch_prepare(FILE *source,
                                                  const EngineProbeResult *profile,
                                                  EngineTlsScratch *scratch)
{
    if (!scratch)
        return ENGINE_TLS_INVALID;
    memset(scratch, 0, sizeof(*scratch));
    if (!source || !profile || profile->status != ENGINE_PROBE_INSPECTED)
        return ENGINE_TLS_INVALID;
    if (!profile->has_tls_segment)
        return ENGINE_TLS_ABSENT;

    const uint64_t initialized = profile->tls_initial_image_bytes;
    const uint64_t total = profile->tls_memory_bytes;
    const uint64_t alignment = profile->tls_alignment > sizeof(void *)
        ? profile->tls_alignment : sizeof(void *);
    if (initialized > total || !alignment || (alignment & (alignment - 1)) ||
        profile->tls_file_offset > UINT64_MAX - initialized ||
        profile->tls_file_offset + initialized > profile->file_bytes)
        return ENGINE_TLS_INVALID;
    if (total > MAX_TLS_TEMPLATE_BYTES || alignment > MAX_TLS_TEMPLATE_BYTES)
        return ENGINE_TLS_TOO_LARGE;
    if (profile->tls_file_offset > LONG_MAX)
        return ENGINE_TLS_INVALID;
    if (!total)
        return ENGINE_TLS_PREPARED;

    // Allocate extra padding so the template meets ELF alignment without
    // relying on a platform-specific aligned allocator. The original malloc
    // pointer is retained for release, and the contents stay non-executable.
    const size_t bytes = (size_t)total;
    const size_t align_bytes = (size_t)alignment;
    if (bytes > SIZE_MAX - (align_bytes - 1))
        return ENGINE_TLS_TOO_LARGE;
    void *allocation = malloc(bytes + align_bytes - 1);
    if (!allocation)
        return ENGINE_TLS_NO_MEMORY;
    const uintptr_t base = (uintptr_t)allocation;
    if (base > UINTPTR_MAX - (align_bytes - 1)) {
        free(allocation);
        return ENGINE_TLS_INVALID;
    }
    unsigned char *data = (unsigned char *)
        ((base + align_bytes - 1) & ~(uintptr_t)(align_bytes - 1));
    memset(data, 0, bytes);

    if (initialized &&
        (fseek(source, (long)profile->tls_file_offset, SEEK_SET) != 0 ||
         fread(data, 1, (size_t)initialized, source) != initialized)) {
        free(allocation);
        return ENGINE_TLS_IO_ERROR;
    }
    scratch->allocation = allocation;
    scratch->data = data;
    scratch->size = bytes;
    scratch->alignment = align_bytes;
    return ENGINE_TLS_PREPARED;
}
