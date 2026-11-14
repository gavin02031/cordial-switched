#include "horizon_code_address.h"

#include <stdint.h>

static bool valid_span_plan(const EngineMemoryPlan *plan)
{
    return plan && plan->image_bytes &&
           plan->image_bytes <= ENGINE_MEMORY_MAX_BYTES &&
           plan->image_bytes % ENGINE_MEMORY_PAGE_BYTES == 0 &&
           plan->virtual_page_base % ENGINE_MEMORY_PAGE_BYTES == 0 &&
           plan->alignment_bytes >= ENGINE_MEMORY_PAGE_BYTES &&
           plan->alignment_bytes <= ENGINE_MEMORY_MAX_ALIGNMENT &&
           (plan->alignment_bytes & (plan->alignment_bytes - 1)) == 0;
}

bool horizon_code_address_search_size(const EngineMemoryPlan *plan,
                                      size_t *search_bytes)
{
    if (!valid_span_plan(plan) || !search_bytes)
        return false;
    const size_t slack = plan->alignment_bytes - ENGINE_MEMORY_PAGE_BYTES;
    if (plan->image_bytes > SIZE_MAX - slack)
        return false;
    *search_bytes = plan->image_bytes + slack;
    return true;
}

bool horizon_code_address_select(const EngineMemoryPlan *plan,
                                 uintptr_t free_span, size_t free_bytes,
                                 uintptr_t *chosen)
{
    size_t needed = 0;
    if (!chosen || !free_span ||
        free_span % ENGINE_MEMORY_PAGE_BYTES != 0 ||
        !horizon_code_address_search_size(plan, &needed) ||
        free_bytes < needed || needed > UINTPTR_MAX - free_span)
        return false;

    const uintptr_t mask = plan->alignment_bytes - 1;
    const uintptr_t phase = (uintptr_t)plan->virtual_page_base & mask;
    const uintptr_t shift = (phase - (free_span & mask)) & mask;
    if (shift > free_bytes - plan->image_bytes)
        return false;
    *chosen = free_span + shift;
    return true;
}
