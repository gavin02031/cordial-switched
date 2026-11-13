#ifndef CORDIAL_HORIZON_CODE_MEMORY_H
#define CORDIAL_HORIZON_CODE_MEMORY_H

#include <stdbool.h>
#include <stddef.h>
#include <switch.h>

#include "engine_memory_plan.h"

typedef struct {
    void *source;
    void *mapped;
    void *reservation;
    size_t size_bytes;
    bool active;
} HorizonCodeMemory;

// Maps fully prepared, page-aligned bytes at a separate Horizon code address.
// Does NOT load an ELF, apply relocations, bind Bionic/JNI, or call any code.
// Relocations must refer to the final mapped base, not the writable source.
// Caller keeps the source allocation live until horizon_code_memory_unmap.
// Mapping must be zero-initialized. On rare rollback failures, a failed map
// can leave active=true so the caller can retry unmap without losing ownership.
// Returns false on any failed syscall and stores the kernel result.
bool horizon_code_memory_map(const EngineMemoryPlan *plan, void *aligned_source,
                             HorizonCodeMemory *mapping, Result *error);
bool horizon_code_memory_unmap(HorizonCodeMemory *mapping, Result *error);

#endif
