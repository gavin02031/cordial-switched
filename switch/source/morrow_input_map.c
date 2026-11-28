#include "morrow_input_map.h"

// Android KeyEvent keycodes (public API constants).
#define AKEY_BACK 4
#define AKEY_DPAD_UP 19
#define AKEY_DPAD_DOWN 20
#define AKEY_DPAD_LEFT 21
#define AKEY_DPAD_RIGHT 22
#define AKEY_DPAD_CENTER 23
#define AKEY_BUTTON_A 96
#define AKEY_BUTTON_B 97
#define AKEY_BUTTON_X 99
#define AKEY_BUTTON_Y 100
#define AKEY_BUTTON_L1 102
#define AKEY_BUTTON_R1 103
#define AKEY_BUTTON_L2 104
#define AKEY_BUTTON_R2 105
#define AKEY_BUTTON_THUMBL 106
#define AKEY_BUTTON_THUMBR 107
#define AKEY_BUTTON_START 108
#define AKEY_BUTTON_SELECT 109

// Android MotionEvent axis ids (public API constants).
#define AAXIS_X 0
#define AAXIS_Y 1
#define AAXIS_Z 11
#define AAXIS_RZ 14

// The guest's key mapper (0x2759248) has one compare-and-return branch per
// honored code; these are the codes with a branch, verified 2026-11-22.
// Anything else returns 0 (unmapped) there, so Morrow must not emit it.
static const int guest_honored_keys[] = {
    AKEY_BACK, // -> 0x41c
    AKEY_DPAD_UP, AKEY_DPAD_DOWN, AKEY_DPAD_LEFT, AKEY_DPAD_RIGHT,
    AKEY_DPAD_CENTER,              // -> 0x41b
    AKEY_BUTTON_A,                 // -> 0x3ea
    AKEY_BUTTON_B,                 // -> 0x3eb
    AKEY_BUTTON_X,                 // -> 0x3e8
    AKEY_BUTTON_Y,                 // -> 0x3e9
    AKEY_BUTTON_L1,                // -> 0x3ed
    AKEY_BUTTON_R1,                // -> 0x27593c8 (L1 sibling branch)
    AKEY_BUTTON_L2,                // -> 0x3ef
    AKEY_BUTTON_R2,                // -> 0x3ee
    AKEY_BUTTON_THUMBL,            // -> 0x3f1
    AKEY_BUTTON_THUMBR,            // -> 0x3f0
    AKEY_BUTTON_START,             // -> 0x3f2
    AKEY_BUTTON_SELECT,            // -> 0x3f3
};

// The guest's axis mapper (0x27593f0): 0->0x3f8, 1->(paired with 0),
// 11->0x3f9, 14->branch sibling, 15/16 are the hat axes the guest splits
// into dpad events itself, 17/22->0x3ef, 18/23->0x3ee. Morrow emits only
// the four stick axes; hats and trigger axes stay unused because the
// digital L2/R2 keys above reach the same guest codes.
static const int guest_honored_axes[] = {
    AAXIS_X, AAXIS_Y, AAXIS_Z, AAXIS_RZ,
};

int morrow_input_map_android_key(morrow_input_button_t button)
{
    switch (button) {
    case MORROW_INPUT_FACE_DOWN: return AKEY_BUTTON_A;
    case MORROW_INPUT_FACE_RIGHT: return AKEY_BUTTON_B;
    case MORROW_INPUT_FACE_LEFT: return AKEY_BUTTON_X;
    case MORROW_INPUT_FACE_TOP: return AKEY_BUTTON_Y;
    case MORROW_INPUT_L1: return AKEY_BUTTON_L1;
    case MORROW_INPUT_R1: return AKEY_BUTTON_R1;
    case MORROW_INPUT_L2: return AKEY_BUTTON_L2;
    case MORROW_INPUT_R2: return AKEY_BUTTON_R2;
    case MORROW_INPUT_STICK_L: return AKEY_BUTTON_THUMBL;
    case MORROW_INPUT_STICK_R: return AKEY_BUTTON_THUMBR;
    case MORROW_INPUT_START: return AKEY_BUTTON_START;
    case MORROW_INPUT_SELECT: return AKEY_BUTTON_SELECT;
    case MORROW_INPUT_DPAD_UP: return AKEY_DPAD_UP;
    case MORROW_INPUT_DPAD_DOWN: return AKEY_DPAD_DOWN;
    case MORROW_INPUT_DPAD_LEFT: return AKEY_DPAD_LEFT;
    case MORROW_INPUT_DPAD_RIGHT: return AKEY_DPAD_RIGHT;
    default: return -1;
    }
}

int morrow_input_map_android_axis(morrow_input_axis_t axis)
{
    switch (axis) {
    case MORROW_INPUT_AXIS_LEFT_X: return AAXIS_X;
    case MORROW_INPUT_AXIS_LEFT_Y: return AAXIS_Y;
    case MORROW_INPUT_AXIS_RIGHT_X: return AAXIS_Z;
    case MORROW_INPUT_AXIS_RIGHT_Y: return AAXIS_RZ;
    default: return -1;
    }
}

bool morrow_input_guest_honors_key(int android_keycode)
{
    for (unsigned i = 0; i < sizeof(guest_honored_keys) / sizeof(guest_honored_keys[0]); ++i) {
        if (guest_honored_keys[i] == android_keycode)
            return true;
    }
    return false;
}

bool morrow_input_guest_honors_axis(int android_axis)
{
    for (unsigned i = 0; i < sizeof(guest_honored_axes) / sizeof(guest_honored_axes[0]); ++i) {
        if (guest_honored_axes[i] == android_axis)
            return true;
    }
    return false;
}
