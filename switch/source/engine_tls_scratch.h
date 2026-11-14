#ifndef CORDIAL_ENGINE_TLS_SCRATCH_H
#define CORDIAL_ENGINE_TLS_SCRATCH_H

#include <stddef.h>
#include <stdio.h>

#include "elf_probe.h"

// PT_TLS supplies bytes for each thread's initialized TLS and the size of
// zero-filled TLS. This scratch copy never becomes the active CPU TLS area.
typedef enum {
    ENGINE_TLS_ABSENT = 0,
    ENGINE_TLS_PREPARED,
    ENGINE_TLS_TOO_LARGE,
    ENGINE_TLS_INVALID,
    ENGINE_TLS_IO_ERROR,
    ENGINE_TLS_NO_MEMORY
} EngineTlsScratchStatus;

typedef struct {
    void *allocation;
    unsigned char *data;
    size_t size;
    size_t alignment;
} EngineTlsScratch;

EngineTlsScratchStatus engine_tls_scratch_prepare(FILE *source,
                                                  const EngineProbeResult *profile,
                                                  EngineTlsScratch *scratch);
void engine_tls_scratch_release(EngineTlsScratch *scratch);

#endif
