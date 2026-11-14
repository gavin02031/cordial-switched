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

typedef enum {
    HORIZON_RESERVE_OK = 0,
    HORIZON_RESERVE_INVALID_PLAN,
    HORIZON_RESERVE_SYSCALL_UNAVAILABLE,
    HORIZON_RESERVE_NO_FREE_SPAN,
    HORIZON_RESERVE_NO_RESERVATION_MEMORY,
    HORIZON_RESERVE_INVALID_STATE
} HorizonCodeReserveStatus;

// Reserve the final code address BEFORE applying address-sensitive ELF
// relocations. The caller can then stage the ELF against mapping->mapped,
// copy the staged bytes to an aligned source, and map the prepared image.
// A reservation is held until unmap, whether or not a mapping is active.
bool horizon_code_memory_reserve(const EngineMemoryPlan *plan,
                                 HorizonCodeMemory *mapping);
bool horizon_code_memory_reserve_detailed(const EngineMemoryPlan *plan,
                                          HorizonCodeMemory *mapping,
                                          HorizonCodeReserveStatus *status);
bool horizon_code_memory_map_reserved(const EngineMemoryPlan *plan,
                                      void *aligned_source,
                                      HorizonCodeMemory *mapping, Result *error);

// Convenience combined API, for callers with position-independent prepared
// bytes. Relocatable ELF callers should use reserve + map_reserved instead.
// Maps fully prepared, page-aligned bytes at a separate Horizon code address.
// Does NOT load an ELF, apply relocations, bind Bionic/JNI, or call any code.
// Relocations must refer to the final mapped base, not the writable source.
// Caller keeps the source allocation live until horizon_code_memory_unmap.
// Mapping must be zero-initialized. On rare rollback failures, a failed map
// can leave active=true so the caller can retry unmap without losing ownership.
// Returns false on any failed syscall and stores the kernel result.
bool horizon_code_memory_map(const EngineMemoryPlan *plan, void *aligned_source,
                             HorizonCodeMemory *mapping, Result *error);
// Also releases a reservation that was never mapped.
bool horizon_code_memory_unmap(HorizonCodeMemory *mapping, Result *error);

#endif
