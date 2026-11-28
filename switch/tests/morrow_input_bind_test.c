#include "morrow_input_bind.h"

#include <assert.h>
#include <stdint.h>

// Bind-time address translation only: discovered exports become entries
// through the same RX-run translation the loader uses, missing or non-RX
// exports stay null, and nothing is invoked. The pump gates are untouched.

#define BASE_PAGE 0x1000u
#define RUNTIME_BASE 0x400000u

static EngineMemoryRun runs[2];

static void make_plan(EngineMemoryPlan *plan)
{
    runs[0] = (EngineMemoryRun){0, 0x4000, ENGINE_MEMORY_READ_EXECUTE};
    runs[1] = (EngineMemoryRun){0x4000, 0x1000, ENGINE_MEMORY_READ_WRITE};
    *plan = (EngineMemoryPlan){0};
    plan->virtual_page_base = BASE_PAGE;
    plan->alignment_bytes = 0x1000;
    plan->image_bytes = 0x5000;
    plan->executable_bytes = 0x4000;
    plan->writable_bytes = 0x1000;
    plan->runs = runs;
    plan->run_count = 2;
}

int main(void)
{
    EngineMemoryPlan plan;
    make_plan(&plan);

    // Full discovery binds all four pump entries at base + (va - page base).
    EngineEntryProbe probe = {0};
    probe.has_gamepad_connect = true;
    probe.gamepad_connect_virtual_address = BASE_PAGE + 0x100;
    probe.has_gamepad_disconnect = true;
    probe.gamepad_disconnect_virtual_address = BASE_PAGE + 0x200;
    probe.has_gamepad_set_supported_key = true;
    probe.gamepad_set_supported_key_virtual_address = BASE_PAGE + 0x300;
    probe.has_gamepad_button_event = true;
    probe.gamepad_button_event_virtual_address = BASE_PAGE + 0x400;

    morrow_input_pump_entries_t entries;
    assert(morrow_input_bind_from_probe(&entries, &probe, &plan, RUNTIME_BASE));
    assert((uintptr_t)entries.gamepad_connect == RUNTIME_BASE + 0x100);
    assert((uintptr_t)entries.gamepad_disconnect == RUNTIME_BASE + 0x200);
    assert((uintptr_t)entries.gamepad_set_supported_key == RUNTIME_BASE + 0x300);
    assert((uintptr_t)entries.gamepad_button_event == RUNTIME_BASE + 0x400);

    // A missing export leaves only that entry null.
    probe.has_gamepad_disconnect = false;
    assert(morrow_input_bind_from_probe(&entries, &probe, &plan, RUNTIME_BASE));
    assert(entries.gamepad_disconnect == NULL);
    assert(entries.gamepad_connect != NULL);

    // An export outside RX runs never becomes a callable entry.
    probe.gamepad_button_event_virtual_address = BASE_PAGE + 0x4500; // RW run
    assert(morrow_input_bind_from_probe(&entries, &probe, &plan, RUNTIME_BASE));
    assert(entries.gamepad_button_event == NULL);
    assert(entries.gamepad_connect != NULL);

    // Nothing discovered binds nothing and reports it.
    EngineEntryProbe empty = {0};
    assert(!morrow_input_bind_from_probe(&entries, &empty, &plan, RUNTIME_BASE));
    assert(entries.gamepad_connect == NULL &&
           entries.gamepad_disconnect == NULL &&
           entries.gamepad_set_supported_key == NULL &&
           entries.gamepad_button_event == NULL);

    // Null arguments refuse without touching the entries.
    assert(!morrow_input_bind_from_probe(NULL, &probe, &plan, RUNTIME_BASE));
    assert(!morrow_input_bind_from_probe(&entries, NULL, &plan, RUNTIME_BASE));
    return 0;
}
