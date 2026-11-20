#ifndef CORDIAL_ENGINE_CONSTRUCTOR_PLAN_H
#define CORDIAL_ENGINE_CONSTRUCTOR_PLAN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "engine_entry_probe.h"
#include "engine_memory_plan.h"
#include "engine_stage.h"

typedef enum {
    ENGINE_CONSTRUCTORS_INVALID = 0,
    ENGINE_CONSTRUCTORS_NO_ENTRIES,
    ENGINE_CONSTRUCTORS_VALIDATED,
    ENGINE_CONSTRUCTORS_BAD_STAGE,
    ENGINE_CONSTRUCTORS_BAD_ARRAY,
    ENGINE_CONSTRUCTORS_BAD_TARGET,
    ENGINE_CONSTRUCTORS_OUT_OF_MEMORY
} EngineConstructorStatus;

typedef struct {
    EngineConstructorStatus status;
    uintptr_t *targets;
    size_t call_count;
    unsigned array_entries;
    unsigned skipped_entries;
    unsigned first_invalid_index;
    bool invalid_dt_init;
    const char *reason;
} EngineConstructorPlan;

// Validate pointers from a *fully relocated* NX staging image against the
// final RX mapping plan. Targets are recorded in Android's DT_INIT then
// DT_INIT_ARRAY order; no guest code is called or made executable here.
// This cannot lift the PT_TLS, constructor execution or JNI startup gates.
bool engine_constructor_plan_prepare(const EngineStage *stage,
                                     const EngineEntryProbe *entry,
                                     const EngineMemoryPlan *memory,
                                     EngineConstructorPlan *result);
void engine_constructor_plan_release(EngineConstructorPlan *plan);

#endif
