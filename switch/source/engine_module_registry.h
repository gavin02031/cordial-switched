#ifndef CORDIAL_ENGINE_MODULE_REGISTRY_H
#define CORDIAL_ENGINE_MODULE_REGISTRY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// The loader's record of guest ELF images it has actually mapped. A guest
// dl_iterate_phdr walks these records, so an entry must only ever describe
// bytes the loader itself placed at a final runtime address - never a
// planned, staged or rolled-back mapping.
#define ENGINE_MODULE_REGISTRY_MAX 8
#define ENGINE_MODULE_NAME_BYTES 64

typedef struct {
    // Relocation delta (final runtime base minus lowest virtual page), as
    // dl_phdr_info.dlpi_addr defines it. Not the first mapped byte.
    uint64_t load_bias;
    const void *program_headers; // Points into the mapped guest image.
    uint16_t program_header_count;
    char name[ENGINE_MODULE_NAME_BYTES];
} EngineModule;

// Registers one mapped guest module. Fails rather than truncating when the
// name does not fit, and never replaces an existing record.
bool engine_module_register(uint64_t load_bias, const void *program_headers,
                            uint16_t program_header_count, const char *name);

// Copies live records into caller storage so an iteration can run its
// callbacks without holding the registry lock (callbacks may re-enter).
// Returns the number of modules copied, capped by capacity.
unsigned engine_module_snapshot(EngineModule *out, unsigned capacity);

// Monotonic registration/removal counters, matching dl_phdr_info's adds and
// subs fields. There is no removal path yet; subs stays zero until one
// exists, rather than reporting a number that cannot happen.
uint64_t engine_module_adds(void);
uint64_t engine_module_subs(void);

void engine_module_registry_reset(void);

#endif
