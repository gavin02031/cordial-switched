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
    // Counts may be partial if malformed input stops the read-only scan.
    unsigned unsupported_relocations_seen;
    unsigned tls_relocations_seen;
    unsigned tlsdesc_relocations_seen;
    // Address of the first mapped ELF page, not the RW staging allocation.
    // Set only when the explicit final-base relocation API was requested.
    uint64_t intended_runtime_base;
    char detail[120];
} EngineStage;

bool engine_stage_stream(FILE *file, EngineStage *stage);
// Resolver must only return real, ABI-compatible symbol addresses. This is a
// non-executable scratch-stage experiment, not a replacement for bionic.
// The provider and version must be checked together. Android's libc and
// libdl imports share some names but their symbol versions are not optional.
// NULL version/library means an unversioned import.
typedef bool (*EngineSymbolResolver)(const char *name, const char *version,
                                     const char *library, uint64_t *address,
                                     void *context);
bool engine_stage_stream_resolved(FILE *file, EngineStage *stage,
                                  EngineSymbolResolver resolver, void *context);
// Prepares supported relocations against a reserved *future* RX mapping,
// while writing only to a separate NX scratch image. No module is executed.
// runtime_base must be page-aligned and at or above the ELF's lowest page.
bool engine_stage_stream_resolved_at(FILE *file, EngineStage *stage,
                                     EngineSymbolResolver resolver, void *context,
                                     uint64_t runtime_base);
// Audit mode continues past missing imports and unsupported relocations in
// non-executable scratch memory, without fabricating ABI-compatible addresses
// or Android TLS. It reports UNSUPPORTED_RELOCATION if any unsupported types
// were seen, otherwise UNRESOLVED_SYMBOL if any import was unavailable.
bool engine_stage_stream_audit(FILE *file, EngineStage *stage,
                               EngineSymbolResolver resolver, void *context);
void engine_stage_release(EngineStage *stage);

#endif
