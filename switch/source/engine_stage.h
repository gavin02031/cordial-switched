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

typedef enum {
    ENGINE_DYNINIT_NO_HEADERS = 0,
    ENGINE_DYNINIT_BAD_HEADERS,
    ENGINE_DYNINIT_NOT_FOUND,
    ENGINE_DYNINIT_INVALID_SECTION,
    ENGINE_DYNINIT_VERIFIED
} EngineDyninitStatus;

typedef struct {
    EngineStageStatus status;
    unsigned char *image;
    size_t image_size;
    uint64_t lowest_virtual_address;
    unsigned rela_applied;
    unsigned relr_applied;
    unsigned packed_applied;
    unsigned symbol_applied;
    // Relocation counts, not unique symbol/provider counts. Absent ELF weak
    // symbols use S=0; they do not imply an implemented Android provider.
    unsigned weak_undefined_relocations;
    unsigned required_missing_relocations;
    // Set only after every relocation table has been scanned. An unresolved
    // import can still make the stage unusable after this point.
    bool relocation_scan_complete;
    // Provider lookup failures include weak imports. Preserve the first
    // mandatory import separately so a hardware report names the real stop.
    char first_required_name[76];
    char first_required_version[45];
    char first_required_library[45];
    // Verified solely from the guest ELF's SHF_ALLOC section and PT_LOAD,
    // never inferred from a missing import or a guessed address.
    bool dyninit_section_verified;
    EngineDyninitStatus dyninit_status;
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
// Same NX audit but with the actual reserved runtime base, so relocation
// validation matches the future executable mapping without jumping into it.
bool engine_stage_stream_audit_at(FILE *file, EngineStage *stage,
                                  EngineSymbolResolver resolver, void *context,
                                  uint64_t runtime_base);
void engine_stage_release(EngineStage *stage);
const char *engine_dyninit_status_name(EngineDyninitStatus status);

#endif
