#ifndef MORROW_ENGINE_EXECUTION_ATTEMPT_H
#define MORROW_ENGINE_EXECUTION_ATTEMPT_H

#include <stdbool.h>
#include <stdint.h>

#include "elf_probe.h"
#include "engine_entry_probe.h"
#include "engine_memory_plan.h"

typedef struct {
    bool eligible;
    uintptr_t target;
    const char *reason;
} EngineExecutionDecision;

// A callable JNI_OnLoad needs much more than a discovered ELF export: every
// relocation must already be bound, the code actually mapped RX, and JNI must
// belong to this thread. Reject PT_TLS/constructors until their Android
// lifecycle is installed; skipping them would execute an invalid programme.
EngineExecutionDecision engine_execution_decide(
    bool explicitly_requested, bool strict_linked, bool mapped,
    const EngineProbeResult *probe, const EngineEntryProbe *entry,
    const EngineMemoryPlan *plan, uint64_t runtime_base,
    bool java_vm_ready, bool java_environment_ready, bool activity_ready);

// Resolve another verified export in the *same* already mapped code image.
// The final mapping must remain alive for the entire guest call.
bool engine_execution_rx_target(const EngineMemoryPlan *plan,
                                uint64_t runtime_base, uint64_t virtual_address,
                                uintptr_t *target);

// Only call following an eligible decision. The caller must keep the mapped
// image and its relocation providers alive after entry returns.
int32_t engine_execution_call_jni_onload(uintptr_t entry, void *java_vm);

#endif
