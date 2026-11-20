#include "engine_tls_scratch.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    unsigned char bytes[128] = {0};
    for (unsigned i = 0; i < 16; i++)
        bytes[32 + i] = (unsigned char)(i + 1);
    FILE *source = tmpfile();
    assert(source);
    assert(fwrite(bytes, 1, sizeof(bytes), source) == sizeof(bytes));

    EngineProbeResult profile = {0};
    profile.status = ENGINE_PROBE_INSPECTED;
    profile.file_bytes = sizeof(bytes);
    EngineTlsScratch first = {0}, second = {0};
    assert(engine_tls_scratch_prepare(source, &profile, &first) == ENGINE_TLS_ABSENT);
    assert(!first.allocation && !first.data);

    profile.has_tls_segment = true;
    profile.tls_file_offset = 32;
    profile.tls_initial_image_bytes = 16;
    profile.tls_memory_bytes = 64;
    profile.tls_alignment = 64;
    assert(engine_tls_scratch_prepare(source, &profile, &first) == ENGINE_TLS_PREPARED);
    assert(first.data && first.size == 64 && first.alignment == 64);
    assert(((uintptr_t)first.data & 63u) == 0);
    for (unsigned i = 0; i < 16; i++)
        assert(first.data[i] == i + 1);
    for (unsigned i = 16; i < 64; i++)
        assert(first.data[i] == 0);

    assert(engine_tls_scratch_prepare(source, &profile, &second) == ENGINE_TLS_PREPARED);
    assert(second.data && first.data != second.data);
    first.data[0] = 0xff;
    assert(second.data[0] == 1); // Each thread needs an independent copy.
    engine_tls_scratch_release(&first);
    assert(first.data == NULL && first.allocation == NULL && first.size == 0);
    engine_tls_scratch_release(&second);
    engine_tls_scratch_release(NULL);

    profile.tls_alignment = 24;
    assert(engine_tls_scratch_prepare(source, &profile, &first) == ENGINE_TLS_INVALID);
    profile.tls_alignment = 4096;
    assert(engine_tls_scratch_prepare(source, &profile, &first) == ENGINE_TLS_PREPARED);
    assert(((uintptr_t)first.data & 4095u) == 0);
    engine_tls_scratch_release(&first);

    profile.tls_initial_image_bytes = 65;
    assert(engine_tls_scratch_prepare(source, &profile, &first) == ENGINE_TLS_INVALID);
    profile.tls_initial_image_bytes = 16;
    profile.tls_file_offset = 120;
    assert(engine_tls_scratch_prepare(source, &profile, &first) == ENGINE_TLS_INVALID);
    profile.tls_file_offset = 32;
    profile.tls_memory_bytes = 2 * 1024 * 1024;
    assert(engine_tls_scratch_prepare(source, &profile, &first) == ENGINE_TLS_TOO_LARGE);
    profile.tls_memory_bytes = 64;
    profile.tls_alignment = 2 * 1024 * 1024;
    assert(engine_tls_scratch_prepare(source, &profile, &first) == ENGINE_TLS_TOO_LARGE);

    profile.tls_alignment = 0;
    profile.tls_memory_bytes = 0;
    profile.tls_initial_image_bytes = 0;
    assert(engine_tls_scratch_prepare(source, &profile, &first) == ENGINE_TLS_PREPARED);
    assert(first.data == NULL);
    profile.tls_initial_image_bytes = 16;
    profile.tls_memory_bytes = 64;
    assert(engine_tls_scratch_prepare(NULL, &profile, &first) == ENGINE_TLS_INVALID);
    assert(engine_tls_scratch_prepare(source, &profile, NULL) == ENGINE_TLS_INVALID);

    // Simulate a file replaced or truncated after the ELF metadata was read.
    profile.tls_file_offset = sizeof(bytes) - 8;
    profile.file_bytes = sizeof(bytes) + 8;
    assert(engine_tls_scratch_prepare(source, &profile, &first) == ENGINE_TLS_IO_ERROR);
    fclose(source);
    puts("engine_tls_scratch_test: aligned copies, BSS, isolation and limits passed");
    return 0;
}
