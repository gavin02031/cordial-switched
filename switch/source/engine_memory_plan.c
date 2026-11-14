#include "engine_memory_plan.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

enum { PAGE_READ = 1, PAGE_WRITE = 2, PAGE_EXECUTE = 4 };

static bool align_up(uint64_t value, uint64_t *out)
{
    if (value > UINT64_MAX - (ENGINE_MEMORY_PAGE_BYTES - 1))
        return false;
    *out = (value + ENGINE_MEMORY_PAGE_BYTES - 1) &
           ~(uint64_t)(ENGINE_MEMORY_PAGE_BYTES - 1);
    return true;
}

void engine_memory_plan_release(EngineMemoryPlan *plan)
{
    if (!plan)
        return;
    free(plan->runs);
    memset(plan, 0, sizeof(*plan));
}

bool engine_memory_plan_validate(const EngineMemoryPlan *plan)
{
    if (!plan || !plan->runs || !plan->run_count || !plan->image_bytes ||
        plan->image_bytes > ENGINE_MEMORY_MAX_BYTES ||
        plan->alignment_bytes < ENGINE_MEMORY_PAGE_BYTES ||
        plan->alignment_bytes > ENGINE_MEMORY_MAX_ALIGNMENT ||
        (plan->alignment_bytes & (plan->alignment_bytes - 1)) ||
        (plan->image_bytes % ENGINE_MEMORY_PAGE_BYTES))
        return false;

    const size_t pages = plan->image_bytes / ENGINE_MEMORY_PAGE_BYTES;
    if (plan->run_count > pages)
        return false;
    unsigned char *covered = calloc(pages, 1);
    if (!covered) return false;
    bool ok = true;
    int previous_order = 0;
    for (size_t i = 0; i < plan->run_count && ok; i++) {
        const EngineMemoryRun *run = &plan->runs[i];
        if (!run->size_bytes || run->offset_bytes % ENGINE_MEMORY_PAGE_BYTES ||
            run->size_bytes % ENGINE_MEMORY_PAGE_BYTES ||
            run->offset_bytes > plan->image_bytes ||
            run->size_bytes > plan->image_bytes - run->offset_bytes ||
            run->permission > ENGINE_MEMORY_READ_WRITE) {
            ok = false;
            break;
        }
        const int order = run->permission == ENGINE_MEMORY_READ_EXECUTE ? 0 :
                          run->permission == ENGINE_MEMORY_READ ? 1 : 2;
        if (i && order < previous_order) {
            ok = false;
            break;
        }
        previous_order = order;
        const size_t start = run->offset_bytes / ENGINE_MEMORY_PAGE_BYTES;
        const size_t count = run->size_bytes / ENGINE_MEMORY_PAGE_BYTES;
        for (size_t p = start; p < start + count; p++) {
            if (covered[p]) {
                ok = false;
                break;
            }
            covered[p] = 1;
        }
    }
    for (size_t p = 0; p < pages && ok; p++)
        if (!covered[p]) ok = false;
    free(covered);
    return ok;
}

bool engine_memory_plan_build(const EngineMemorySegment *segments, size_t count,
                              EngineMemoryPlan *plan)
{
    if (!plan)
        return false;
    memset(plan, 0, sizeof(*plan));
    if (!segments || !count || count > ENGINE_PROBE_MAX_LOAD_SEGMENTS)
        return false;

    uint64_t lowest = UINT64_MAX, highest = 0;
    for (size_t i = 0; i < count; i++) {
        const EngineMemorySegment *seg = &segments[i];
        if (!seg->memory_bytes || (seg->executable && seg->writable) ||
            seg->virtual_address > UINT64_MAX - seg->memory_bytes)
            return false;
        const uint64_t start = seg->virtual_address &
                               ~(uint64_t)(ENGINE_MEMORY_PAGE_BYTES - 1);
        uint64_t end;
        if (!align_up(seg->virtual_address + seg->memory_bytes, &end))
            return false;
        if (start < lowest) lowest = start;
        if (end > highest) highest = end;
    }
    if (highest <= lowest || highest - lowest > ENGINE_MEMORY_MAX_BYTES)
        return false;

    const size_t size = (size_t)(highest - lowest);
    const size_t pages = size / ENGINE_MEMORY_PAGE_BYTES;
    unsigned char *flags = calloc(pages, 1);
    EngineMemoryRun *runs = calloc(pages, sizeof(*runs));
    if (!flags || !runs) {
        free(flags);
        free(runs);
        return false;
    }
    bool ok = true;
    for (size_t i = 0; i < count && ok; i++) {
        const EngineMemorySegment *seg = &segments[i];
        const size_t first = (size_t)((seg->virtual_address - lowest) /
                                      ENGINE_MEMORY_PAGE_BYTES);
        uint64_t end;
        if (!align_up(seg->virtual_address + seg->memory_bytes, &end)) {
            ok = false;
            break;
        }
        const size_t last = (size_t)((end - lowest) / ENGINE_MEMORY_PAGE_BYTES);
        const unsigned char bits = PAGE_READ |
                                   (seg->writable ? PAGE_WRITE : 0) |
                                   (seg->executable ? PAGE_EXECUTE : 0);
        for (size_t p = first; p < last; p++) {
            flags[p] |= bits;
            if ((flags[p] & (PAGE_WRITE | PAGE_EXECUTE)) ==
                (PAGE_WRITE | PAGE_EXECUTE)) {
                ok = false;
                break;
            }
        }
    }

    if (ok) {
        // RX first from the freshly-mapped state: no W->X transition.
        // Holes between PT_LOADs stay R (not executable or writable).
        const EngineMemoryPermission ordered[] = {
            ENGINE_MEMORY_READ_EXECUTE, ENGINE_MEMORY_READ,
            ENGINE_MEMORY_READ_WRITE
        };
        for (size_t pass = 0; pass < sizeof(ordered) / sizeof(ordered[0]); pass++) {
            const EngineMemoryPermission permission = ordered[pass];
            for (size_t p = 0; p < pages;) {
                const EngineMemoryPermission actual =
                    (flags[p] & PAGE_EXECUTE) ? ENGINE_MEMORY_READ_EXECUTE :
                    (flags[p] & PAGE_WRITE) ? ENGINE_MEMORY_READ_WRITE :
                    ENGINE_MEMORY_READ;
                if (actual != permission) {
                    p++;
                    continue;
                }
                size_t end = p + 1;
                while (end < pages) {
                    const EngineMemoryPermission next =
                        (flags[end] & PAGE_EXECUTE) ? ENGINE_MEMORY_READ_EXECUTE :
                        (flags[end] & PAGE_WRITE) ? ENGINE_MEMORY_READ_WRITE :
                        ENGINE_MEMORY_READ;
                    if (next != permission) break;
                    end++;
                }
                runs[plan->run_count++] = (EngineMemoryRun){
                    p * ENGINE_MEMORY_PAGE_BYTES,
                    (end - p) * ENGINE_MEMORY_PAGE_BYTES,
                    permission
                };
                switch (permission) {
                case ENGINE_MEMORY_READ_EXECUTE:
                    plan->executable_bytes += (end - p) * ENGINE_MEMORY_PAGE_BYTES;
                    break;
                case ENGINE_MEMORY_READ_WRITE:
                    plan->writable_bytes += (end - p) * ENGINE_MEMORY_PAGE_BYTES;
                    break;
                case ENGINE_MEMORY_READ:
                    plan->readonly_bytes += (end - p) * ENGINE_MEMORY_PAGE_BYTES;
                    break;
                }
                p = end;
            }
        }
        plan->virtual_page_base = lowest;
        plan->alignment_bytes = ENGINE_MEMORY_PAGE_BYTES;
        plan->image_bytes = size;
        plan->runs = runs;
    } else {
        free(runs);
    }
    free(flags);
    return ok;
}

bool engine_memory_plan_from_probe(const EngineProbeResult *probe,
                                   EngineMemoryPlan *plan)
{
    if (!plan) return false;
    if (!probe || probe->status != ENGINE_PROBE_INSPECTED ||
        !probe->load_segments ||
        probe->load_segments > ENGINE_PROBE_MAX_LOAD_SEGMENTS) {
        memset(plan, 0, sizeof(*plan));
        return false;
    }
    EngineMemorySegment segments[ENGINE_PROBE_MAX_LOAD_SEGMENTS];
    for (size_t i = 0; i < probe->load_segments; i++) {
        segments[i] = (EngineMemorySegment){
            probe->load[i].virtual_address,
            probe->load[i].memory_bytes,
            (probe->load[i].flags & 1u) != 0,
            (probe->load[i].flags & 2u) != 0
        };
    }
    if (!engine_memory_plan_build(segments, probe->load_segments, plan))
        return false;
    if (probe->max_segment_alignment > ENGINE_MEMORY_PAGE_BYTES) {
        if (probe->max_segment_alignment > ENGINE_MEMORY_MAX_ALIGNMENT) {
            engine_memory_plan_release(plan);
            return false;
        }
        plan->alignment_bytes = (size_t)probe->max_segment_alignment;
    }
    if (!engine_memory_plan_validate(plan)) {
        engine_memory_plan_release(plan);
        return false;
    }
    return true;
}
