#ifndef CORDIAL_SWITCH_ENGINE_STAGE_H
#define CORDIAL_SWITCH_ENGINE_STAGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

// Bounded scratch image, deliberately never made executable. The live loader
// still needs Horizon code-memory mapping, symbol binding and bionic/JNI.
#define ENGINE_STAGE_MAX_BYTES (192u * 1024u * 1024u)

typedef enum {
    ENGINE_STAGE_INVALID = 0,
    ENGINE_STAGE_TOO_LARGE,
    ENGINE_STAGE_IO_ERROR,
    ENGINE_STAGE_UNSUPPORTED_RELOCATION,
    ENGINE_STAGE_UNRESOLVED_SYMBOL,
    ENGINE_STAGE_PREPARED
} EngineStageStatus;

typedef struct {
    EngineStageStatus status;
    unsigned char *image;
    size_t image_size;
    uint64_t lowest_virtual_address;
    unsigned rela_applied;
    unsigned relr_applied;
    unsigned packed_applied;
    unsigned symbol_applied;
    char detail[120];
} EngineStage;

bool engine_stage_stream(FILE *file, EngineStage *stage);
// Resolver must only return real, ABI-compatible symbol addresses. This is a
// non-executable scratch-stage experiment, not a replacement for bionic.
typedef bool (*EngineSymbolResolver)(const char *name, uint64_t *address, void *context);
bool engine_stage_stream_resolved(FILE *file, EngineStage *stage,
                                  EngineSymbolResolver resolver, void *context);
void engine_stage_release(EngineStage *stage);

#endif
