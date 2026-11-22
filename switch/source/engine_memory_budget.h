#ifndef CORDIAL_SWITCH_ENGINE_MEMORY_BUDGET_H
#define CORDIAL_SWITCH_ENGINE_MEMORY_BUDGET_H

#include <stddef.h>
#include <stdint.h>

// Pure formatter for a pre-guest memory budget line. The caller supplies
// numbers it read (svcGetInfo total/used, engine file size, readability
// flags); this code only does arithmetic and text. It does not call the
// kernel and executes no guest code.
//
// The verdict is arithmetic, not a prediction: file size is only a lower
// bound on the mapped image (bss, heap, thread stacks and guest allocations
// come on top), so "fits" means "the file alone is smaller than free
// memory", never "the engine will run".
typedef enum {
    ENGINE_MEMORY_BUDGET_UNKNOWN = 0,      // a needed number was unavailable
    ENGINE_MEMORY_BUDGET_FILE_EXCEEDS_FREE,
    ENGINE_MEMORY_BUDGET_FILE_BELOW_FREE
} EngineMemoryBudgetVerdict;

typedef struct {
    int total_ok;
    uint64_t total_bytes;
    int used_ok;
    uint64_t used_bytes;
    int file_ok;
    uint64_t file_bytes;
} EngineMemoryBudgetInput;

EngineMemoryBudgetVerdict engine_memory_budget_verdict(
    const EngineMemoryBudgetInput *in);

// Writes one "key=value ..." line without a trailing newline. Returns the
// length that would be written (snprintf semantics). Unavailable numbers
// print as "unavailable", never as zero.
size_t engine_memory_budget_format(const EngineMemoryBudgetInput *in,
                                   char *out, size_t out_size);

#endif
