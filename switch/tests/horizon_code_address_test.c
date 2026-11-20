#include "horizon_code_address.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

int main(void)
{
    EngineMemoryPlan plan = {
        .virtual_page_base = 0,
        .alignment_bytes = 16384,
        .image_bytes = 124858368,
    };
    size_t span_bytes = 0;
    assert(horizon_code_address_search_size(&plan, &span_bytes));
    assert(span_bytes == plan.image_bytes + 12288);

    // libnx's free-span search is only 4 KiB-aligned. Check all four ASLR
    // phases so a successful test cannot be an accidental 1-in-4 alignment.
    for (uintptr_t phase = 0; phase < 16384; phase += 4096) {
        uintptr_t chosen = 0;
        const uintptr_t free_span = UINT64_C(0x20000000) + phase;
        assert(horizon_code_address_select(&plan, free_span, span_bytes, &chosen));
        assert(chosen >= free_span);
        assert(chosen + plan.image_bytes <= free_span + span_bytes);
        assert(chosen % 16384 == 0);
        assert(chosen - free_span < 16384);
    }

    // ELF load bias sometimes requires a nonzero p_align phase. A randomly
    // chosen 4 KiB address must still yield the correct one every time.
    plan.virtual_page_base = 4096;
    for (uintptr_t phase = 0; phase < 16384; phase += 4096) {
        uintptr_t chosen = 0;
        const uintptr_t free_span = UINT64_C(0x30000000) + phase;
        assert(horizon_code_address_select(&plan, free_span, span_bytes, &chosen));
        assert(chosen % plan.alignment_bytes == 4096);
        assert(chosen + plan.image_bytes <= free_span + span_bytes);
    }

    uintptr_t ignored = 0;
    assert(!horizon_code_address_select(&plan, 0x20001000, span_bytes - 4096, &ignored));
    assert(!horizon_code_address_select(&plan, 0x20000123, span_bytes, &ignored));
    assert(!horizon_code_address_select(&plan, 0, span_bytes, &ignored));
    assert(!horizon_code_address_select(&plan, 0x20000000, span_bytes, NULL));

    plan.alignment_bytes = 4096;
    plan.virtual_page_base = 0;
    assert(horizon_code_address_search_size(&plan, &span_bytes));
    assert(span_bytes == plan.image_bytes);
    assert(horizon_code_address_select(&plan, 0x40001000, span_bytes, &ignored));
    assert(ignored == 0x40001000);

    plan.alignment_bytes = ENGINE_MEMORY_MAX_ALIGNMENT;
    assert(horizon_code_address_search_size(&plan, &span_bytes));
    assert(span_bytes == plan.image_bytes + ENGINE_MEMORY_MAX_ALIGNMENT - 4096);
    assert(horizon_code_address_select(&plan, 0x40001000, span_bytes, &ignored));
    assert(ignored % ENGINE_MEMORY_MAX_ALIGNMENT == 0);

    plan.alignment_bytes = 3;
    assert(!horizon_code_address_search_size(&plan, &span_bytes));
    plan.alignment_bytes = ENGINE_MEMORY_MAX_ALIGNMENT * 2;
    assert(!horizon_code_address_search_size(&plan, &span_bytes));
    plan.alignment_bytes = 4096;
    plan.image_bytes = 0;
    assert(!horizon_code_address_search_size(&plan, &span_bytes));
    assert(!horizon_code_address_search_size(NULL, &span_bytes));
    assert(!horizon_code_address_search_size(&plan, NULL));

    puts("horizon_code_address_test: all 4 KiB ASLR phases and ELF load biases passed");
    return 0;
}
