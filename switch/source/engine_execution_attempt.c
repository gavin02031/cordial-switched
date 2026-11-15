#include "engine_execution_attempt.h"

#include <limits.h>
#include <stddef.h>

static EngineExecutionDecision blocked(const char *reason)
{
    return (EngineExecutionDecision){false, 0, reason};
}

bool engine_execution_rx_target(const EngineMemoryPlan *plan,
                                uint64_t runtime_base, uint64_t virtual_address,
                                uintptr_t *target)
{
    if (!target || !plan || !engine_memory_plan_validate(plan) ||
        virtual_address < plan->virtual_page_base || runtime_base > UINTPTR_MAX)
        return false;
    const uint64_t offset = virtual_address - plan->virtual_page_base;
    if (offset >= plan->image_bytes || offset > UINTPTR_MAX - runtime_base)
        return false;
    for (size_t i = 0; i < plan->run_count; i++) {
        const EngineMemoryRun *run = &plan->runs[i];
        if (run->permission == ENGINE_MEMORY_READ_EXECUTE &&
            offset >= run->offset_bytes &&
            offset - run->offset_bytes < run->size_bytes) {
            *target = (uintptr_t)(runtime_base + offset);
            return true;
        }
    }
    return false;
}

EngineExecutionDecision engine_execution_decide(
    bool explicitly_requested, bool strict_linked, bool mapped,
    const EngineProbeResult *probe, const EngineEntryProbe *entry,
    const EngineMemoryPlan *plan, uint64_t runtime_base,
    bool java_vm_ready, bool java_environment_ready, bool activity_ready)
{
    if (!explicitly_requested) return blocked("not_requested");
    if (!strict_linked) return blocked("unresolved_imports_or_relocations");
    if (!probe || !entry || !plan || !mapped)
        return blocked("engine_not_mapped_rx");
    if (entry->status != ENGINE_ENTRY_INSPECTED || !entry->has_jni_onload)
        return blocked("jni_onload_export_unknown_or_missing");
    if (probe->has_tls_segment)
        return blocked("android_thread_tls_not_installed");
    if (entry->has_init || entry->has_init_array)
        return blocked("elf_constructors_not_run");
    if (!java_vm_ready || !java_environment_ready || !activity_ready)
        return blocked("jni_game_activity_runtime_incomplete");
    if (!engine_memory_plan_validate(plan))
        return blocked("invalid_executable_memory_plan");
    uintptr_t target = 0;
    if (!engine_execution_rx_target(plan, runtime_base,
                                    entry->jni_onload_virtual_address, &target))
        return blocked("jni_onload_not_in_rx_pages");
    return (EngineExecutionDecision){true, target,
                                     "jni_onload_eligible_experimental"};
}

int32_t engine_execution_call_jni_onload(uintptr_t entry, void *java_vm)
{
    // JNI_OnLoad has a stable JavaVM*, void* ABI on Android AArch64. This
    // deliberately does not guess GameActivity.initializeNativeCode's ABI.
    int32_t (*onload)(void *, void *) =
        (int32_t (*)(void *, void *))entry;
    return onload(java_vm, NULL);
}
