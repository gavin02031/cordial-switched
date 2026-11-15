#include "engine_execution_attempt.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static unsigned calls;
static int32_t synthetic_onload(void *vm, void *reserved)
{
    assert(vm == &calls && reserved == NULL);
    calls++;
    return 0x00010006;
}

int main(void)
{
    const EngineMemorySegment segments[] = {
        {0x1000, 0x1000, true, false},
        {0x2000, 0x1000, false, true}
    };
    EngineMemoryPlan plan = {0};
    assert(engine_memory_plan_build(segments, 2, &plan));
    EngineProbeResult probe = {0};
    EngineEntryProbe entry = {0};
    entry.status = ENGINE_ENTRY_INSPECTED;
    entry.has_jni_onload = true;
    entry.jni_onload_virtual_address = 0x1100;

#define DECIDE(requested, linked, mapped, vm, env, activity) \
    engine_execution_decide(requested, linked, mapped, &probe, &entry, &plan, \
                            0x200000, vm, env, activity)
    EngineExecutionDecision result = DECIDE(false, true, true, true, true, true);
    assert(!result.eligible && !strcmp(result.reason, "not_requested"));
    result = DECIDE(true, false, true, true, true, true);
    assert(!result.eligible &&
           !strcmp(result.reason, "unresolved_imports_or_relocations"));
    result = DECIDE(true, true, false, true, true, true);
    assert(!result.eligible && !strcmp(result.reason, "engine_not_mapped_rx"));

    probe.has_tls_segment = true;
    result = DECIDE(true, true, true, true, true, true);
    assert(!result.eligible &&
           !strcmp(result.reason, "android_thread_tls_not_installed"));
    probe.has_tls_segment = false;
    entry.has_init_array = true;
    result = DECIDE(true, true, true, true, true, true);
    assert(!result.eligible && !strcmp(result.reason, "elf_constructors_not_run"));
    entry.has_init_array = false;
    result = DECIDE(true, true, true, false, true, true);
    assert(!result.eligible &&
           !strcmp(result.reason, "jni_game_activity_runtime_incomplete"));

    entry.jni_onload_virtual_address = 0x2100;
    result = DECIDE(true, true, true, true, true, true);
    assert(!result.eligible && !strcmp(result.reason, "jni_onload_not_in_rx_pages"));
    entry.jni_onload_virtual_address = 0x1100;
    result = DECIDE(true, true, true, true, true, true);
    assert(result.eligible && result.target == 0x200100);

    // Synthetic JNI only: verifies the exact two-argument ABI of the call
    // boundary without loading or distributing any Roblox code.
    assert(engine_execution_call_jni_onload((uintptr_t)synthetic_onload, &calls)
           == 0x00010006);
    assert(calls == 1);
    engine_memory_plan_release(&plan);
    puts("engine_execution_attempt_test: all assertions passed");
    return 0;
}
