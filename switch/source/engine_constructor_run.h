#ifndef CORDIAL_ENGINE_CONSTRUCTOR_RUN_H
#define CORDIAL_ENGINE_CONSTRUCTOR_RUN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "engine_constructor_plan.h"

// A controlled state machine for a dependency/constructor runner.
// Nothing here maps executable code, enters an Android ELF, or sets up TLS.
// Live integration MUST supply a gate that proves mapping and thread-state
// readiness; a guest TLS bridge is installed only when the ELF carries a
// PT_TLS template.
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
// and refuse blind re-execution. The live call site is horizon_real_link.c's
// milestone-B path: mapping pinned, crash marker durable, guest TLS bridge
// installed when the ELF has a TLS template.
bool engine_constructor_run_once(const EngineConstructorPlan *plan,
                                 EngineConstructorRunState *state,
                                 EngineConstructorRunGate gate,
                                 EngineConstructorRunJournal begin,
                                 EngineConstructorRunJournal completed,
                                 EngineConstructorRunCall call,
                                 void *context);

// Entry readiness for the engine entry point after the constructor phase.
// A plan with no entries is trivially ready. Otherwise the run must have
// been attempted, must have completed every constructor, and its crash
// marker must have cleared durably: a partial or undurable run leaves the
// engine's globals in an unknown state and is never entry readiness.
bool engine_constructor_run_entry_ready(const EngineConstructorPlan *plan,
                                        const EngineConstructorRunState *state,
                                        bool attempted, bool marker_cleared);

#endif
