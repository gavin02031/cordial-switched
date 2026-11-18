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
    return 0;
}
