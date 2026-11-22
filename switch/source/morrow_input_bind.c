#include "morrow_input_bind.h"

#include <stddef.h>

#include "engine_execution_attempt.h"

static bool translate(const EngineMemoryPlan *plan, uint64_t runtime_base,
                      bool discovered, uint64_t virtual_address,
                      uintptr_t *out)
{
    *out = 0;
    if (!discovered)
        return false;
    return engine_execution_rx_target(plan, runtime_base, virtual_address,
                                      out);
}

bool morrow_input_bind_from_probe(morrow_input_pump_entries_t *entries,
                                  const EngineEntryProbe *probe,
                                  const EngineMemoryPlan *plan,
                                  uint64_t runtime_base)
{
    if (!entries || !probe)
        return false;
    *entries = (morrow_input_pump_entries_t){0};
    uintptr_t target = 0;
    bool any = false;

    if (translate(plan, runtime_base, probe->has_gamepad_connect,
                  probe->gamepad_connect_virtual_address, &target)) {
        entries->gamepad_connect =
            (void (*)(void *, void *, int32_t, int32_t))target;
        any = true;
    }
    if (translate(plan, runtime_base, probe->has_gamepad_disconnect,
                  probe->gamepad_disconnect_virtual_address, &target)) {
        entries->gamepad_disconnect =
            (void (*)(void *, void *, int32_t, int32_t))target;
        any = true;
    }
    if (translate(plan, runtime_base, probe->has_gamepad_set_supported_key,
                  probe->gamepad_set_supported_key_virtual_address, &target)) {
        entries->gamepad_set_supported_key =
            (void (*)(void *, void *, int32_t, int32_t, bool, int32_t))target;
        any = true;
    }
    if (translate(plan, runtime_base, probe->has_gamepad_button_event,
                  probe->gamepad_button_event_virtual_address, &target)) {
        entries->gamepad_button_event =
            (void (*)(void *, void *, int32_t, int32_t, int32_t))target;
        any = true;
    }
    return any;
}
