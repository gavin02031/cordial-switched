#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "engine_constructor_plan.h"
#include "engine_constructor_run.h"
#include "engine_execution_attempt.h"
#include "engine_guest_tpidr.h"

// Host-side verification of the milestone-B execution machinery: the TPIDR
// bridge models install/restore symmetry, and the constructor runner drives
// native fake targets in plan order with durable begin/completed markers.
// Nothing here executes guest code or models real thread-pointer behavior.

static int calls[8];
static unsigned call_count;
static char markers[8][32];
static unsigned marker_count;

static void fake_ctor_a(int argc, char **argv, char **envp)
{
    (void)envp;
    // The Android argument triple for a guest library is the honest zero.
    assert(argc == 0 && argv == NULL);
    calls[call_count++] = 1;
}

static void fake_ctor_b(int argc, char **argv, char **envp)
{
    (void)argc; (void)argv; (void)envp;
    calls[call_count++] = 2;
}

static void fake_ctor_c(int argc, char **argv, char **envp)
{
    (void)argc; (void)argv; (void)envp;
    calls[call_count++] = 3;
}

static bool gate_allow(void *context)
{
    bool *allowed = context;
    return *allowed;
}

static bool journal_marker(size_t index, uintptr_t target, void *context)
{
    (void)context;
    assert(target != 0 && (target & 3u) == 0);
    snprintf(markers[marker_count], sizeof(markers[0]), "%s%zu",
             (marker_count % 2 == 0) ? "begin" : "done", index);
    marker_count++;
    return true;
}

static void run_call(uintptr_t target, void *context)
{
    (void)context;
    engine_execution_call_constructor(target);
}

// Integration-path model context: the mapping-pinned-only gate the target
// wiring uses for an ELF without PT_TLS, plus completed-marker failure
// injection.
typedef struct {
    bool pinned;
    size_t fail_completed_at; // SIZE_MAX disables failure injection
} IntegrationContext;

static bool gate_pinned_only(void *context)
{
    return ((IntegrationContext *)context)->pinned;
}

static bool completed_with_failure(size_t index, uintptr_t target, void *context)
{
    IntegrationContext *ctx = context;
    if (index == ctx->fail_completed_at)
        return false;
    return journal_marker(index, target, context);
}

int main(void)
{
    // Bridge symmetry on the host model.
    int token = 0;
    assert(engine_guest_tpidr_installed_for_test() == NULL);
    uint64_t saved = engine_guest_tpidr_install(&token);
    assert(engine_guest_tpidr_installed_for_test() == &token);
    engine_guest_tpidr_restore(saved);
    assert(engine_guest_tpidr_installed_for_test() == NULL);

    uintptr_t targets[] = {
        (uintptr_t)(void *)fake_ctor_a,
        (uintptr_t)(void *)fake_ctor_b,
        (uintptr_t)(void *)fake_ctor_c,
    };
    EngineConstructorPlan plan = {0};
    plan.status = ENGINE_CONSTRUCTORS_VALIDATED;
    plan.targets = targets;
    plan.call_count = 3;

    // A denied gate consumes nothing: no attempt, no state change.
    bool allowed = false;
    EngineConstructorRunState state = {0};
    assert(!engine_constructor_run_once(&plan, &state, gate_allow,
                                        journal_marker, journal_marker,
                                        run_call, &allowed));
    assert(!state.started && call_count == 0 && marker_count == 0);

    // Allowed: every target runs exactly once, in order, each wrapped in a
    // begin marker before the call and a completed marker after.
    allowed = true;
    assert(engine_constructor_run_once(&plan, &state, gate_allow,
                                       journal_marker, journal_marker,
                                       run_call, &allowed));
    assert(state.started && state.completed && state.next_index == 3);
    assert(call_count == 3);
    assert(calls[0] == 1 && calls[1] == 2 && calls[2] == 3);
    assert(marker_count == 6);
    assert(strcmp(markers[0], "begin0") == 0);
    assert(strcmp(markers[5], "done2") == 0);

    // A consumed state machine never re-runs, matching the no-blind-retry
    // contract a crash journal depends on.
    assert(!engine_constructor_run_once(&plan, &state, gate_allow,
                                        journal_marker, journal_marker,
                                        run_call, &allowed));
    assert(call_count == 3);

    printf("engine_guest_tpidr_test: all assertions passed\n");

    // Integration-path model of horizon_real_link.c's milestone-B sequence
    // for an ELF WITHOUT a PT_TLS template (libroblox.so): no bridge is
    // installed, the constructor gate is mapping-pinned only, and JNI_OnLoad
    // permission comes from the entry-readiness rule over the actual run.
    // This exercises the same decision chain the target wiring follows; the
    // wiring itself compiles target-only and stays console-validated.

    // No-TLS scenario, full success: constructors run without any bridge and
    // JNI entry is permitted.
    {
        IntegrationContext ctx = { true, SIZE_MAX };
        call_count = 0;
        marker_count = 0;
        // Mirror of the target wiring: engine_main_tls_ready == false means
        // no install call happens at all.
        assert(engine_guest_tpidr_installed_for_test() == NULL);
        EngineConstructorRunState run = {0};
        const bool attempted = true;
        const bool done = engine_constructor_run_once(
            &plan, &run, gate_pinned_only,
            journal_marker, journal_marker, run_call, &ctx);
        const bool marker_cleared = true; // journal finish succeeded
        assert(done && run.completed && call_count == 3);
        assert(engine_guest_tpidr_installed_for_test() == NULL);
        assert(engine_constructor_run_entry_ready(&plan, &run, attempted,
                                                  marker_cleared));
    }
    // No-TLS scenario, completed-marker fails after constructor 1 ran: a
    // partial run is real execution (constructors_ran) but never readiness.
    {
        IntegrationContext ctx = { true, 1 };
        call_count = 0;
        marker_count = 0;
        EngineConstructorRunState run = {0};
        const bool done = engine_constructor_run_once(
            &plan, &run, gate_pinned_only,
            journal_marker, completed_with_failure, run_call, &ctx);
        assert(!done && run.started && !run.completed && run.next_index == 1);
        assert(call_count == 2); // ctors 0 and 1 executed before the failure
        assert(!engine_constructor_run_entry_ready(&plan, &run, true, true));
        // Even a completed run with a stuck marker is not readiness.
        run.completed = true;
        assert(!engine_constructor_run_entry_ready(&plan, &run, true, false));
    }
    // Mapping not pinned: the gate refuses, nothing is attempted, entry
    // stays closed.
    {
        IntegrationContext ctx = { false, SIZE_MAX };
        call_count = 0;
        marker_count = 0;
        EngineConstructorRunState run = {0};
        assert(!engine_constructor_run_once(
            &plan, &run, gate_pinned_only,
            journal_marker, journal_marker, run_call, &ctx));
        assert(!run.started && call_count == 0);
        assert(!engine_constructor_run_entry_ready(&plan, &run, false, false));
    }
    printf("engine_guest_tpidr_test: no-TLS integration path scenarios passed\n");
    return 0;
}
