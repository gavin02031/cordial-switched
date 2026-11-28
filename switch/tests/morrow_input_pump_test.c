#include "morrow_input_pump.h"

#include <assert.h>

#include "morrow_input_map.h"
#include <stdint.h>

// Gate verification for the inactive input wiring: emits must refuse until
// the pump is LIVE, must never call a null entry, and must never emit a
// code outside the guest-honored set. These tests exercise Morrow's own
// gating only; they say nothing about guest behavior.

static int connect_calls;
static int button_calls;
static int32_t last_device, last_code, last_action;

static void fake_connect(void *env, void *cls, int32_t device, int32_t type)
{
    (void)env; (void)cls; (void)type;
    ++connect_calls;
    last_device = device;
}

static void fake_button(void *env, void *cls, int32_t device, int32_t code, int32_t action)
{
    (void)env; (void)cls;
    ++button_calls;
    last_device = device;
    last_code = code;
    last_action = action;
}

int main(void)
{
    morrow_input_pump_t pump;
    morrow_input_pump_init(&pump);
    assert(morrow_input_pump_state(&pump) == MORROW_INPUT_PUMP_INACTIVE);

    // Inactive: nothing is delivered, readiness alone changes nothing.
    assert(!morrow_input_pump_emit_connect(&pump, 0, 1));
    morrow_input_pump_note_engine_ready(&pump);
    assert(morrow_input_pump_state(&pump) == MORROW_INPUT_PUMP_INACTIVE);
    assert(!morrow_input_pump_emit_connect(&pump, 0, 1));

    // Readiness noted while INACTIVE persists: binding then goes straight
    // to LIVE (the flag is the readiness record, not the state).
    morrow_input_pump_entries_t entries = {0};
    entries.gamepad_connect = fake_connect;
    entries.gamepad_button_event = fake_button;
    morrow_input_pump_bind(&pump, &entries);
    assert(morrow_input_pump_state(&pump) == MORROW_INPUT_PUMP_LIVE);

    // Fresh pump: binding before readiness arms but does not activate.
    morrow_input_pump_t pump2;
    morrow_input_pump_init(&pump2);
    morrow_input_pump_bind(&pump2, &entries);
    assert(morrow_input_pump_state(&pump2) == MORROW_INPUT_PUMP_ARMED);
    assert(!morrow_input_pump_emit_connect(&pump2, 0, 1));
    assert(connect_calls == 0);
    morrow_input_pump_note_engine_ready(&pump2);
    assert(morrow_input_pump_state(&pump2) == MORROW_INPUT_PUMP_LIVE);

    // Live: bound entries fire with validated arguments; unbound refuse.
    assert(morrow_input_pump_emit_connect(&pump, 7, 1));
    assert(connect_calls == 1 && last_device == 7);
    assert(!morrow_input_pump_emit_disconnect(&pump, 7, 1));
    // INFERRED action encoding is not executable: LIVE + bound entries
    // alone do not deliver buttons until the encoding is resolved.
    assert(!morrow_input_pump_emit_button(&pump, 7, MORROW_INPUT_FACE_DOWN, true));
    assert(button_calls == 0);
    morrow_input_pump_note_button_action_encoding_resolved(&pump);
    assert(morrow_input_pump_emit_button(&pump, 7, MORROW_INPUT_FACE_DOWN, true));
    assert(button_calls == 1 && last_device == 7 && last_code == 96 && last_action == 0);
    assert(morrow_input_pump_emit_button(&pump, 7, MORROW_INPUT_START, false));
    assert(button_calls == 2 && last_code == 108 && last_action == 1);

    // Out-of-range controls never reach the guest entry.
    assert(!morrow_input_pump_emit_button(&pump, 7, 999, true));
    assert(button_calls == 2);
    // Gamepad type outside the guest's 1..3 enum is refused.
    assert(!morrow_input_pump_emit_supported_key(&pump, 7, MORROW_INPUT_FACE_DOWN, true, 0));
    assert(!morrow_input_pump_emit_supported_key(&pump, 7, MORROW_INPUT_FACE_DOWN, true, 4));

    // Stop is terminal; later readiness and binds cannot revive it.
    morrow_input_pump_stop(&pump);
    assert(morrow_input_pump_state(&pump) == MORROW_INPUT_PUMP_STOPPED);
    assert(!morrow_input_pump_emit_connect(&pump, 7, 1));
    assert(connect_calls == 1);
    morrow_input_pump_bind(&pump, &entries);
    assert(morrow_input_pump_state(&pump) == MORROW_INPUT_PUMP_STOPPED);

    // A stopped pump with resolved semantics still refuses buttons.
    assert(!morrow_input_pump_emit_button(&pump, 7, MORROW_INPUT_FACE_DOWN, true));
    assert(button_calls == 2);
    return 0;
}
