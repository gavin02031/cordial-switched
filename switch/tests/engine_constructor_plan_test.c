#include "engine_constructor_plan.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void write_pointer(unsigned char *image, size_t offset, uint64_t value)
{
    memcpy(image + offset, &value, sizeof(value));
}

int main(void)
{
    const EngineMemorySegment segments[] = {
        {0x1000, 0x1000, true, false},
        {0x2000, 0x1000, false, true},
    };
    EngineMemoryPlan memory = {0};
    assert(engine_memory_plan_build(segments, 2, &memory));

    EngineStage stage = {0};
    stage.status = ENGINE_STAGE_PREPARED;
    stage.lowest_virtual_address = memory.virtual_page_base;
    stage.image_size = memory.image_bytes;
    stage.intended_runtime_base = 0x400000;
    stage.image = calloc(1, stage.image_size);
    assert(stage.image);

    EngineEntryProbe entry = {0};
    entry.status = ENGINE_ENTRY_INSPECTED;
    entry.has_init = true;
    entry.init_virtual_address = 0x1100;
    entry.has_init_array = true;
    entry.init_array_virtual_address = 0x2100;
    entry.init_array_entries = 4;
    const size_t array = 0x1100;
    write_pointer(stage.image, array, 0);
    write_pointer(stage.image, array + 8, UINT64_MAX);
    write_pointer(stage.image, array + 16, 0x400108);
    write_pointer(stage.image, array + 24, 0x400200);

    EngineConstructorPlan verified = {0};
    assert(engine_constructor_plan_prepare(&stage, &entry, &memory, &verified));
    assert(verified.status == ENGINE_CONSTRUCTORS_VALIDATED);
    assert(verified.array_entries == 4 && verified.skipped_entries == 2);
    assert(verified.call_count == 3);
    assert(verified.targets[0] == 0x400100);
    assert(verified.targets[1] == 0x400108);
    assert(verified.targets[2] == 0x400200);
    engine_constructor_plan_release(&verified);
    assert(!verified.targets);

    // A relocated pointer into guest data, a non-4-byte-aligned function,
    // or an address outside RX must block the entire plan atomically.
    write_pointer(stage.image, array + 24, 0x401100);
    assert(!engine_constructor_plan_prepare(&stage, &entry, &memory, &verified));
    assert(verified.status == ENGINE_CONSTRUCTORS_BAD_TARGET);
    assert(verified.first_invalid_index == 3 && !verified.targets);
    write_pointer(stage.image, array + 24, 0x400202);
    assert(!engine_constructor_plan_prepare(&stage, &entry, &memory, &verified));
    assert(verified.status == ENGINE_CONSTRUCTORS_BAD_TARGET);
    write_pointer(stage.image, array + 24, 0x900000);
    assert(!engine_constructor_plan_prepare(&stage, &entry, &memory, &verified));
    assert(verified.status == ENGINE_CONSTRUCTORS_BAD_TARGET);
    write_pointer(stage.image, array + 24, 0x400200);

    entry.init_virtual_address = 0x2100;
    assert(!engine_constructor_plan_prepare(&stage, &entry, &memory, &verified));
    assert(verified.status == ENGINE_CONSTRUCTORS_BAD_TARGET);
    assert(verified.invalid_dt_init);
    entry.init_virtual_address = 0x1100;

    entry.init_array_virtual_address = 0x2ff8;
    assert(!engine_constructor_plan_prepare(&stage, &entry, &memory, &verified));
    assert(verified.status == ENGINE_CONSTRUCTORS_BAD_ARRAY);
    entry.init_array_virtual_address = 0x2100;

    stage.intended_runtime_base = 0;
    assert(!engine_constructor_plan_prepare(&stage, &entry, &memory, &verified));
    assert(verified.status == ENGINE_CONSTRUCTORS_BAD_STAGE);
    stage.intended_runtime_base = 0x400000;
    stage.status = ENGINE_STAGE_UNRESOLVED_SYMBOL;
    assert(!engine_constructor_plan_prepare(&stage, &entry, &memory, &verified));
    assert(verified.status == ENGINE_CONSTRUCTORS_BAD_STAGE);
    stage.status = ENGINE_STAGE_PREPARED;

    entry.has_init = false;
    entry.init_virtual_address = 0;
    entry.has_init_array = false;
    entry.init_array_virtual_address = 0;
    entry.init_array_entries = 0;
    assert(engine_constructor_plan_prepare(&stage, &entry, &memory, &verified));
    assert(verified.status == ENGINE_CONSTRUCTORS_NO_ENTRIES && !verified.targets);
    engine_constructor_plan_release(&verified);

    free(stage.image);
    engine_memory_plan_release(&memory);
    puts("engine_constructor_plan_test: all assertions passed (no guest execution)");
    return 0;
}
