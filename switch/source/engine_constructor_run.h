#ifndef CORDIAL_ENGINE_CONSTRUCTOR_RUN_H
#define CORDIAL_ENGINE_CONSTRUCTOR_RUN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "engine_constructor_plan.h"

// A controlled state machine for a future dependency/constructor runner.
// Nothing here maps executable code, enters an Android ELF, or sets up TLS.
// Live integration MUST supply a gate that proves mapping, Android TLS,
// dependency ordering, and thread-state readiness.
typedef struct {
    bool started;
    bool completed;
    size_t next_index;
} EngineConstructorRunState;

typedef bool (*EngineConstructorRunGate)(void *context);
typedef bool (*EngineConstructorRunJournal)(size_t index, uintptr_t target,
                                            void *context);
typedef void (*EngineConstructorRunCall)(uintptr_t target, void *context);

// For each target: durable begin marker -> caller -> durable completed marker.
// A failed gate makes no attempt. Any attempt, including a failed journal,
// consumes the state and cannot be retried in the same process. A crash between
// markers is intentionally unresolved; restarting must read the durable journal
// and refuse blind re-execution. This unit has NO live guest call site yet.
bool engine_constructor_run_once(const EngineConstructorPlan *plan,
                                 EngineConstructorRunState *state,
                                 EngineConstructorRunGate gate,
                                 EngineConstructorRunJournal begin,
                                 EngineConstructorRunJournal completed,
                                 EngineConstructorRunCall call,
                                 void *context);

#endif
