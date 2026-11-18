#include "engine_constructor_run.h"

bool engine_constructor_run_once(const EngineConstructorPlan *plan,
                                 EngineConstructorRunState *state,
                                 EngineConstructorRunGate gate,
                                 EngineConstructorRunJournal begin,
                                 EngineConstructorRunJournal completed,
                                 EngineConstructorRunCall call,
                                 void *context)
{
    if (!plan || !state || state->started || !gate || !begin || !completed || !call ||
        (plan->status != ENGINE_CONSTRUCTORS_VALIDATED &&
         plan->status != ENGINE_CONSTRUCTORS_NO_ENTRIES) ||
        (plan->status == ENGINE_CONSTRUCTORS_NO_ENTRIES && plan->call_count) ||
        (plan->call_count && !plan->targets))
        return false;

    // The live caller must prove the executable mapping is pinned, the
    // dependency graph is initialized and the guest thread/TLS is ready.
    if (!gate(context)) return false;
    state->started = true;

    for (size_t i = 0; i < plan->call_count; ++i) {
        const uintptr_t target = plan->targets[i];
        if (!target || (target & 3u)) return false;
        if (!begin(i, target, context)) return false;
        call(target, context);
        if (!completed(i, target, context)) return false;
        state->next_index = i + 1;
    }

    state->completed = true;
    return true;
}

bool engine_constructor_run_entry_ready(const EngineConstructorPlan *plan,
                                        const EngineConstructorRunState *state,
                                        bool attempted, bool marker_cleared)
{
    if (!plan)
        return false;
    if (plan->status == ENGINE_CONSTRUCTORS_NO_ENTRIES || !plan->call_count)
        return true;
    if (!attempted || !state)
        return false;
    return state->completed && marker_cleared;
}
