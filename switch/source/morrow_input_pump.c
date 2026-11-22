#include "morrow_input_pump.h"

#include <stddef.h>

#include "morrow_input_map.h"

// Android key event actions (public API), used for button events. The
// guest passes the value through to its own record; the encoding is
// INFERRED from the Android contract until consumer-verified.
#define MORROW_ACTION_DOWN 0
#define MORROW_ACTION_UP 1

void morrow_input_pump_init(morrow_input_pump_t *pump)
{
    if (!pump)
        return;
    pump->state = MORROW_INPUT_PUMP_INACTIVE;
    pump->entries = (morrow_input_pump_entries_t){0};
    pump->engine_ready = false;
    pump->button_action_encoding_resolved = false;
}

void morrow_input_pump_bind(morrow_input_pump_t *pump,
                            const morrow_input_pump_entries_t *entries)
{
    if (!pump || !entries)
        return;
    if (pump->state != MORROW_INPUT_PUMP_INACTIVE)
        return;
    pump->entries = *entries;
    if (entries->gamepad_connect || entries->gamepad_disconnect ||
        entries->gamepad_set_supported_key || entries->gamepad_button_event) {
        pump->state = pump->engine_ready ? MORROW_INPUT_PUMP_LIVE : MORROW_INPUT_PUMP_ARMED;
    }
}

void morrow_input_pump_note_engine_ready(morrow_input_pump_t *pump)
{
    if (!pump)
        return;
    pump->engine_ready = true;
    if (pump->state == MORROW_INPUT_PUMP_ARMED)
        pump->state = MORROW_INPUT_PUMP_LIVE;
}

void morrow_input_pump_stop(morrow_input_pump_t *pump)
{
    if (!pump)
        return;
    pump->state = MORROW_INPUT_PUMP_STOPPED;
}

morrow_input_pump_state_t morrow_input_pump_state(const morrow_input_pump_t *pump)
{
    return pump ? pump->state : MORROW_INPUT_PUMP_INACTIVE;
}

void morrow_input_pump_note_button_action_encoding_resolved(morrow_input_pump_t *pump)
{
    if (!pump)
        return;
    pump->button_action_encoding_resolved = true;
}

static bool pump_live(const morrow_input_pump_t *pump)
{
    return pump && pump->state == MORROW_INPUT_PUMP_LIVE && pump->engine_ready;
}

bool morrow_input_pump_emit_connect(morrow_input_pump_t *pump,
                                    int32_t device_id, int32_t gamepad_type)
{
    if (!pump_live(pump) || !pump->entries.gamepad_connect)
        return false;
    pump->entries.gamepad_connect(NULL, NULL, device_id, gamepad_type);
    return true;
}

bool morrow_input_pump_emit_disconnect(morrow_input_pump_t *pump,
                                       int32_t device_id, int32_t gamepad_type)
{
    if (!pump_live(pump) || !pump->entries.gamepad_disconnect)
        return false;
    pump->entries.gamepad_disconnect(NULL, NULL, device_id, gamepad_type);
    return true;
}

bool morrow_input_pump_emit_supported_key(morrow_input_pump_t *pump,
                                          int32_t device_id, int morrow_button,
                                          bool supported, int32_t gamepad_type)
{
    if (!pump_live(pump) || !pump->entries.gamepad_set_supported_key)
        return false;
    int code = morrow_input_map_android_key((morrow_input_button_t)morrow_button);
    if (code < 0 || !morrow_input_guest_honors_key(code))
        return false;
    if (gamepad_type < 1 || gamepad_type > 3)
        return false; // the guest clamps the type enum to 1..3
    pump->entries.gamepad_set_supported_key(NULL, NULL, device_id, code,
                                            supported, gamepad_type);
    return true;
}

bool morrow_input_pump_emit_button(morrow_input_pump_t *pump,
                                   int32_t device_id, int morrow_button,
                                   bool pressed)
{
    if (!pump_live(pump) || !pump->entries.gamepad_button_event)
        return false;
    // INFERRED encoding is not executable: require the explicit
    // consumer-verified resolution note on top of LIVE + bound entries.
    if (!pump->button_action_encoding_resolved)
        return false;
    int code = morrow_input_map_android_key((morrow_input_button_t)morrow_button);
    if (code < 0 || !morrow_input_guest_honors_key(code))
        return false;
    pump->entries.gamepad_button_event(NULL, NULL, device_id, code,
                                       pressed ? MORROW_ACTION_DOWN : MORROW_ACTION_UP);
    return true;
}
