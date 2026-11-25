#include "engine_memory_budget.h"

#include <inttypes.h>
#include <stdio.h>

EngineMemoryBudgetVerdict engine_memory_budget_verdict(
    const EngineMemoryBudgetInput *in)
{
    if (!in || !in->total_ok || !in->used_ok || !in->file_ok ||
        in->used_bytes > in->total_bytes)
        return ENGINE_MEMORY_BUDGET_UNKNOWN;
    return in->file_bytes > in->total_bytes - in->used_bytes
               ? ENGINE_MEMORY_BUDGET_FILE_EXCEEDS_FREE
               : ENGINE_MEMORY_BUDGET_FILE_BELOW_FREE;
}

static const char *verdict_text(EngineMemoryBudgetVerdict v)
{
    switch (v) {
    case ENGINE_MEMORY_BUDGET_FILE_EXCEEDS_FREE: return "file_exceeds_kernel_uncommitted_not_oom";
    case ENGINE_MEMORY_BUDGET_FILE_BELOW_FREE: return "file_below_kernel_uncommitted_not_a_fit_claim";
    default: return "unknown";
    }
}

size_t engine_memory_budget_format(const EngineMemoryBudgetInput *in,
                                   char *out, size_t out_size)
{
    char total[32] = "unavailable", used[32] = "unavailable",
         file[32] = "unavailable", freeb[32] = "unavailable";
    if (in && in->total_ok)
        snprintf(total, sizeof total, "%" PRIu64, in->total_bytes);
    if (in && in->used_ok)
        snprintf(used, sizeof used, "%" PRIu64, in->used_bytes);
    if (in && in->file_ok)
        snprintf(file, sizeof file, "%" PRIu64, in->file_bytes);
    if (in && in->total_ok && in->used_ok && in->used_bytes <= in->total_bytes)
        snprintf(freeb, sizeof freeb, "%" PRIu64,
                 in->total_bytes - in->used_bytes);
    return (size_t)snprintf(out, out_size,
        "memory_budget kernel_total=%s kernel_used=%s kernel_uncommitted=%s engine_file=%s verdict=%s allocation_gate=informational_only",
        total, used, freeb, file, verdict_text(engine_memory_budget_verdict(in)));
}
