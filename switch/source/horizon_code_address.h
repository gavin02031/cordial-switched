#ifndef CORDIAL_HORIZON_CODE_ADDRESS_H
#define CORDIAL_HORIZON_CODE_ADDRESS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "engine_memory_plan.h"

// libnx virtmemFindCodeMemory(size, guard_size) returns a page-aligned free
// span. Its second argument is NOT an ELF alignment request. Allocate up to
// p_align - one page of slack and select the correct load-bias phase within it.
// This pure helper can be tested on the host without Horizon syscalls.
bool horizon_code_address_search_size(const EngineMemoryPlan *plan,
                                      size_t *search_bytes);
bool horizon_code_address_select(const EngineMemoryPlan *plan,
                                 uintptr_t free_span, size_t free_bytes,
                                 uintptr_t *chosen);

#endif
