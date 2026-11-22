#ifndef CORDIAL_SWITCH_MORROW_INPUT_PUMP_H
#define CORDIAL_SWITCH_MORROW_INPUT_PUMP_H

#include <stdbool.h>
#include <stdint.h>

// Inactive-by-construction input wiring toward the guest's exported input
// natives. This module owns gating and argument validation only: it never
// resolves guest symbols itself and never calls a null entry point.
//
// Load-bearing limits, established 2026-11-22 and NOT to be weakened in
// callers (see switch/docs/morrow-v099-import-census.md, input section):
// - The guest post path load-then-increfs the stream delegate; the guest
//   side of that acquisition window is UNPROVEN. This module's stop/join
//   ordering covers Morrow-initiated teardown only; the guest may replace
//   or release delegates internally at times we have not traced.
// - The GameActivity-ready handshake proves that handshake, not consumer
//   readiness; it is the best anchored signal we have and is used as the
//   readiness gate, labeled as such.
// - The guest event streams are single-producer by design: exactly one
//   thread may ever call morrow_input_pump_emit_* for a live pump.
// - Axis events stay INACTIVE: the float payload layout of
//   nativeGamepadAxisEvent is unresolved. Button action encoding follows
//   Android (0 = down, 1 = up), marked INFERRED until consumer-verified.

typedef struct {
    // Resolved guest entry points; null means unresolved, which forces the
    // corresponding emit path to refuse. Filled by the loader at
    // activation time, never by this module.
    void (*gamepad_connect)(void *env, void *cls, int32_t device_id, int32_t gamepad_type);
    void (*gamepad_disconnect)(void *env, void *cls, int32_t device_id, int32_t gamepad_type);
    void (*gamepad_set_supported_key)(void *env, void *cls, int32_t device_id,
                                      int32_t android_keycode, bool supported,
                                      int32_t gamepad_type);
    void (*gamepad_button_event)(void *env, void *cls, int32_t device_id,
                                 int32_t android_keycode, int32_t action);
} morrow_input_pump_entries_t;

typedef enum {
    MORROW_INPUT_PUMP_INACTIVE = 0, // no entries bound, emits refuse
    MORROW_INPUT_PUMP_ARMED,        // entries bound, waiting for readiness
    MORROW_INPUT_PUMP_LIVE,         // readiness observed, emits allowed
    MORROW_INPUT_PUMP_STOPPED       // terminal; a pump is never restarted
} morrow_input_pump_state_t;

typedef struct {
    morrow_input_pump_state_t state;
    morrow_input_pump_entries_t entries;
    bool engine_ready;
} morrow_input_pump_t;

void morrow_input_pump_init(morrow_input_pump_t *pump);

// Bind resolved entries (any may be null). Moves INACTIVE -> ARMED when at
// least one entry is non-null. Has no effect once LIVE or STOPPED.
void morrow_input_pump_bind(morrow_input_pump_t *pump,
                            const morrow_input_pump_entries_t *entries);

// Record the engine-ready signal (the GameActivity handshake anchor).
void morrow_input_pump_note_engine_ready(morrow_input_pump_t *pump);

// Stop the pump. Terminal. Callers must join the single producer thread
// before the guest engine stops; see the limits above.
void morrow_input_pump_stop(morrow_input_pump_t *pump);

morrow_input_pump_state_t morrow_input_pump_state(const morrow_input_pump_t *pump);

// Emit helpers. Each returns true only when the event was delivered to the
// bound guest entry. All refuse unless the pump is LIVE, the entry is
// bound, and (for buttons) the control maps to a guest-honored code.
bool morrow_input_pump_emit_connect(morrow_input_pump_t *pump,
                                    int32_t device_id, int32_t gamepad_type);
bool morrow_input_pump_emit_disconnect(morrow_input_pump_t *pump,
                                       int32_t device_id, int32_t gamepad_type);
bool morrow_input_pump_emit_supported_key(morrow_input_pump_t *pump,
                                          int32_t device_id, int morrow_button,
                                          bool supported, int32_t gamepad_type);
bool morrow_input_pump_emit_button(morrow_input_pump_t *pump,
                                   int32_t device_id, int morrow_button,
                                   bool pressed);

#endif
