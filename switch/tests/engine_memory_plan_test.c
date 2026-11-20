#include "engine_memory_plan.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

int main(void)
{
    EngineMemoryPlan plan;
    const EngineMemorySegment normal[] = {
        {0x1000, 0x1ff0, true, false},
        {0x4000, 0x1000, false, false},
        {0x5000, 0x1c80, false, true}
    };
    assert(engine_memory_plan_build(normal, 3, &plan));
    assert(engine_memory_plan_validate(&plan));
    assert(plan.virtual_page_base == 0x1000);
    assert(plan.alignment_bytes == ENGINE_MEMORY_PAGE_BYTES);
    assert(plan.image_bytes == 0x6000);
    assert(plan.run_count == 3);
    assert(plan.runs[0].permission == ENGINE_MEMORY_READ_EXECUTE);
    assert(plan.runs[0].offset_bytes == 0);
    assert(plan.runs[0].size_bytes == 0x2000);
    assert(plan.runs[1].permission == ENGINE_MEMORY_READ);
    assert(plan.runs[1].offset_bytes == 0x2000);
    assert(plan.runs[1].size_bytes == 0x2000);
    assert(plan.runs[2].permission == ENGINE_MEMORY_READ_WRITE);
    assert(plan.runs[2].offset_bytes == 0x4000);
    assert(plan.runs[2].size_bytes == 0x2000);
    assert(plan.executable_bytes == 0x2000);
    assert(plan.readonly_bytes == 0x2000);
    assert(plan.writable_bytes == 0x2000);
    plan.runs[2].offset_bytes = plan.runs[0].offset_bytes;
    assert(!engine_memory_plan_validate(&plan)); // Duplicate pages, missing RW.
    plan.runs[2].offset_bytes = 0x4000;
    assert(engine_memory_plan_validate(&plan));
    engine_memory_plan_release(&plan);

    EngineProbeResult probe = {0};
    probe.status = ENGINE_PROBE_INSPECTED;
    probe.load_segments = 3;
    for (unsigned i = 0; i < 3; i++) {
        probe.load[i].virtual_address = normal[i].virtual_address;
        probe.load[i].memory_bytes = normal[i].memory_bytes;
        probe.load[i].flags = normal[i].executable ? 5u :
                              normal[i].writable ? 6u : 4u;
    }
    assert(engine_memory_plan_from_probe(&probe, &plan));
    assert(plan.run_count == 3 && plan.image_bytes == 0x6000);
    engine_memory_plan_release(&plan);
    probe.max_segment_alignment = 16384;
    assert(engine_memory_plan_from_probe(&probe, &plan));
    assert(plan.alignment_bytes == 16384);
    assert(engine_memory_plan_validate(&plan));
    engine_memory_plan_release(&plan);
    probe.max_segment_alignment = ENGINE_MEMORY_MAX_ALIGNMENT * 2u;
    assert(!engine_memory_plan_from_probe(&probe, &plan));
    probe.max_segment_alignment = 0;
    probe.load[0].flags = 7;
    assert(!engine_memory_plan_from_probe(&probe, &plan));
    probe.status = ENGINE_PROBE_INVALID_ELF;
    assert(!engine_memory_plan_from_probe(&probe, &plan));

    const EngineMemorySegment shared_page[] = {
        {0x1000, 0x0ff0, true, false},
        {0x1ff0, 0x200, false, true}
    };
    assert(!engine_memory_plan_build(shared_page, 2, &plan));
    assert(plan.runs == NULL);

    const EngineMemorySegment wx = {0, 0x1000, true, true};
    assert(!engine_memory_plan_build(&wx, 1, &plan));
    const EngineMemorySegment too_large = {0, ENGINE_MEMORY_MAX_BYTES + 1, false, true};
    assert(!engine_memory_plan_build(&too_large, 1, &plan));
    const EngineMemorySegment overflow = {UINT64_MAX - 7, 16, false, true};
    assert(!engine_memory_plan_build(&overflow, 1, &plan));
    const EngineMemorySegment zero = {0x1000, 0, false, true};
    assert(!engine_memory_plan_build(&zero, 1, &plan));
    assert(!engine_memory_plan_build(NULL, 0, &plan));

    // Page flags can be fragmented; the output always groups contiguous
    // pages and applies all RX runs before every RW run.
    const EngineMemorySegment fragment[] = {
        {0x1000, 0x1000, false, true},
        {0x2000, 0x1000, true, false},
        {0x3000, 0x1000, false, true},
        {0x4000, 0x1000, true, false}
    };
    assert(engine_memory_plan_build(fragment, 4, &plan));
    assert(plan.run_count == 4);
    assert(engine_memory_plan_validate(&plan));
    assert(plan.runs[0].offset_bytes == 0x1000);
    assert(plan.runs[1].offset_bytes == 0x3000);
    assert(plan.runs[2].offset_bytes == 0);
    assert(plan.runs[3].offset_bytes == 0x2000);
    engine_memory_plan_release(&plan);
    puts("engine_memory_plan_test: all assertions passed");
    return 0;
}
