#ifndef CORDIAL_SWITCH_MORROW_INPUT_MAP_H
#define CORDIAL_SWITCH_MORROW_INPUT_MAP_H

#include <stdbool.h>

// Abstract pad controls, named by function so the wiring layer can bind any
// physical controller. This module only maps identifiers: it contains no
// guest calls, no payload encoding and no event emission. The Android
// key/axis constants are public Android API; the honored set is verified
// against the guest binary's own translation code (libroblox key mapper at
// 0x2759248, axis mapper at 0x27593f0 - see
// switch/docs/morrow-v099-import-census.md, input section).

typedef enum {
    MORROW_INPUT_FACE_DOWN = 0, // bottom face button
    MORROW_INPUT_FACE_RIGHT,    // right face button
    MORROW_INPUT_FACE_LEFT,     // left face button
    MORROW_INPUT_FACE_TOP,      // top face button
    MORROW_INPUT_L1,
    MORROW_INPUT_R1,
    MORROW_INPUT_L2,            // digital trigger (Switch ZL/ZR are digital)
    MORROW_INPUT_R2,
    MORROW_INPUT_STICK_L,       // left stick click
    MORROW_INPUT_STICK_R,       // right stick click
    MORROW_INPUT_START,         // +
    MORROW_INPUT_SELECT,        // -
    MORROW_INPUT_DPAD_UP,
    MORROW_INPUT_DPAD_DOWN,
    MORROW_INPUT_DPAD_LEFT,
    MORROW_INPUT_DPAD_RIGHT,
    MORROW_INPUT_BUTTON_COUNT
} morrow_input_button_t;

typedef enum {
    MORROW_INPUT_AXIS_LEFT_X = 0,
    MORROW_INPUT_AXIS_LEFT_Y,
    MORROW_INPUT_AXIS_RIGHT_X,
    MORROW_INPUT_AXIS_RIGHT_Y,
    MORROW_INPUT_AXIS_COUNT
} morrow_input_axis_t;

// Android KeyEvent keycode for a control, or -1 when unmapped.
int morrow_input_map_android_key(morrow_input_button_t button);

// Android MotionEvent axis id for a stick axis, or -1 when unmapped.
int morrow_input_map_android_axis(morrow_input_axis_t axis);

// True when the guest's verified key mapper honors this Android keycode.
bool morrow_input_guest_honors_key(int android_keycode);

// True when the guest's verified axis mapper honors this Android axis id.
bool morrow_input_guest_honors_axis(int android_axis);

#endif
