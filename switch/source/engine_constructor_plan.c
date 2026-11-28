#include "engine_constructor_plan.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

enum { MAX_CONSTRUCTOR_ARRAY_ENTRIES = 8192 };

static bool reject(EngineConstructorPlan *result, EngineConstructorStatus status,
                   const char *reason)
{
    free(result->targets);
    result->targets = NULL;
    result->call_count = 0;
    result->status = status;
    result->reason = reason;
    return false;
}

void engine_constructor_plan_release(EngineConstructorPlan *plan)
{
    if (!plan) return;
    free(plan->targets);
    memset(plan, 0, sizeof(*plan));
}

// The memory plan is validated once before traversing thousands of entries.
// engine_execution_rx_target revalidates the entire plan per target, which
// would do needless page allocations for the 2,505 constructors seen on Lite.
static bool in_rx(const EngineMemoryPlan *memory, uint64_t runtime_base,
                  uint64_t target)
{
    if (runtime_base > UINTPTR_MAX || target > UINTPTR_MAX ||
        target < runtime_base || (target & 3u)) return false;
    const uint64_t offset = target - runtime_base;
    if (offset >= memory->image_bytes) return false;
    for (size_t i = 0; i < memory->run_count; ++i) {
        const EngineMemoryRun *run = &memory->runs[i];
        if (run->permission == ENGINE_MEMORY_READ_EXECUTE &&
            offset >= run->offset_bytes &&
            offset - run->offset_bytes < run->size_bytes)
            return true;
    }
    return false;
}

bool engine_constructor_plan_prepare(const EngineStage *stage,
                                     const EngineEntryProbe *entry,
                                     const EngineMemoryPlan *memory,
                                     EngineConstructorPlan *result)
{
    if (!result) return false;
    memset(result, 0, sizeof(*result));
    if (!stage || !entry || !memory || !stage->image ||
        stage->status != ENGINE_STAGE_PREPARED ||
        entry->status != ENGINE_ENTRY_INSPECTED ||
        !engine_memory_plan_validate(memory) ||
        stage->lowest_virtual_address != memory->virtual_page_base ||
        stage->image_size != memory->image_bytes ||
        !stage->intended_runtime_base ||
        stage->intended_runtime_base > UINTPTR_MAX ||
        stage->intended_runtime_base % memory->alignment_bytes !=
            memory->virtual_page_base % memory->alignment_bytes)
        return reject(result, ENGINE_CONSTRUCTORS_BAD_STAGE,
                      "strict_final_base_stage_required");

    if (entry->has_init_array != (entry->init_array_entries != 0) ||
        entry->init_array_entries > MAX_CONSTRUCTOR_ARRAY_ENTRIES ||
        (entry->has_init && !entry->init_virtual_address) ||
        (!entry->has_init && entry->init_virtual_address) ||
        (!entry->has_init_array && entry->init_array_virtual_address))
        return reject(result, ENGINE_CONSTRUCTORS_INVALID,
                      "invalid_constructor_metadata");
    result->array_entries = entry->init_array_entries;
    if (!entry->has_init && !entry->has_init_array) {
        result->status = ENGINE_CONSTRUCTORS_NO_ENTRIES;
        result->reason = "no_elf_constructors";
        return true;
    }

    size_t array_offset = 0;
    if (entry->has_init_array) {
        if ((entry->init_array_virtual_address & 7u) ||
            entry->init_array_virtual_address < memory->virtual_page_base)
            return reject(result, ENGINE_CONSTRUCTORS_BAD_ARRAY,
                          "array_alignment_or_base_invalid");
        const uint64_t delta = entry->init_array_virtual_address -
                               memory->virtual_page_base;
        const size_t bytes = (size_t)entry->init_array_entries * sizeof(uint64_t);
        if (delta > stage->image_size || bytes > stage->image_size - delta)
            return reject(result, ENGINE_CONSTRUCTORS_BAD_ARRAY,
                          "array_outside_staged_image");
        array_offset = (size_t)delta;
    }

    const size_t capacity = entry->init_array_entries + (entry->has_init ? 1u : 0u);
    result->targets = calloc(capacity, sizeof(*result->targets));
    if (!result->targets)
        return reject(result, ENGINE_CONSTRUCTORS_OUT_OF_MEMORY,
                      "constructor_plan_allocation_failed");

    if (entry->has_init) {
        if (entry->init_virtual_address < memory->virtual_page_base)
            return reject(result, ENGINE_CONSTRUCTORS_BAD_TARGET,
                          "dt_init_before_image_base");
        const uint64_t offset = entry->init_virtual_address - memory->virtual_page_base;
        const uint64_t base = stage->intended_runtime_base;
        if (offset > UINT64_MAX - base || !in_rx(memory, base, base + offset)) {
            result->invalid_dt_init = true;
            return reject(result, ENGINE_CONSTRUCTORS_BAD_TARGET,
                          "dt_init_not_in_executable_mapping");
        }
        result->targets[result->call_count++] = (uintptr_t)(base + offset);
    }

    for (unsigned i = 0; i < entry->init_array_entries; ++i) {
        uint64_t target = 0;
        memcpy(&target, stage->image + array_offset + (size_t)i * sizeof(target),
               sizeof(target));
        // Bionic's linker explicitly skips the null and -1 sentinels.
        if (target == 0 || target == UINT64_MAX) {
            result->skipped_entries++;
            continue;
        }
        if (!in_rx(memory, stage->intended_runtime_base, target)) {
            result->first_invalid_index = i;
            return reject(result, ENGINE_CONSTRUCTORS_BAD_TARGET,
                          "init_array_target_not_in_executable_mapping");
        }
        result->targets[result->call_count++] = (uintptr_t)target;
    }
    result->status = ENGINE_CONSTRUCTORS_VALIDATED;
    result->reason = "relocated_constructor_targets_validated_no_execution";
    return true;
}
