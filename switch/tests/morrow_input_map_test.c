#include "morrow_input_map.h"

#include <assert.h>

// Verifies the position-based pad mapping stays inside the set of Android
// codes the guest binary's own translation branches honor, so the wiring
// layer can never emit a code the guest would drop.
int main(void)
{
    // Spot checks: standard pad layout in the public Android code domain.
    assert(morrow_input_map_android_key(MORROW_INPUT_FACE_DOWN) == 96);
    assert(morrow_input_map_android_key(MORROW_INPUT_FACE_RIGHT) == 97);
    assert(morrow_input_map_android_key(MORROW_INPUT_FACE_LEFT) == 99);
    assert(morrow_input_map_android_key(MORROW_INPUT_FACE_TOP) == 100);
    assert(morrow_input_map_android_key(MORROW_INPUT_L1) == 102);
    assert(morrow_input_map_android_key(MORROW_INPUT_R1) == 103);
    assert(morrow_input_map_android_key(MORROW_INPUT_L2) == 104);
    assert(morrow_input_map_android_key(MORROW_INPUT_R2) == 105);
    assert(morrow_input_map_android_key(MORROW_INPUT_STICK_L) == 106);
    assert(morrow_input_map_android_key(MORROW_INPUT_STICK_R) == 107);
    assert(morrow_input_map_android_key(MORROW_INPUT_START) == 108);
    assert(morrow_input_map_android_key(MORROW_INPUT_SELECT) == 109);
    assert(morrow_input_map_android_key(MORROW_INPUT_DPAD_UP) == 19);
    assert(morrow_input_map_android_key(MORROW_INPUT_DPAD_DOWN) == 20);
    assert(morrow_input_map_android_key(MORROW_INPUT_DPAD_LEFT) == 21);
    assert(morrow_input_map_android_key(MORROW_INPUT_DPAD_RIGHT) == 22);
    assert(morrow_input_map_android_axis(MORROW_INPUT_AXIS_LEFT_X) == 0);
    assert(morrow_input_map_android_axis(MORROW_INPUT_AXIS_LEFT_Y) == 1);
    assert(morrow_input_map_android_axis(MORROW_INPUT_AXIS_RIGHT_X) == 11);
    assert(morrow_input_map_android_axis(MORROW_INPUT_AXIS_RIGHT_Y) == 14);

    // Every emitted code must be one the guest's verified mapper honors.
    for (int b = 0; b < MORROW_INPUT_BUTTON_COUNT; ++b) {
        int code = morrow_input_map_android_key((morrow_input_button_t)b);
        assert(code >= 0);
        assert(morrow_input_guest_honors_key(code));
    }
    for (int a = 0; a < MORROW_INPUT_AXIS_COUNT; ++a) {
        int axis = morrow_input_map_android_axis((morrow_input_axis_t)a);
        assert(axis >= 0);
        assert(morrow_input_guest_honors_axis(axis));
    }

    // Codes the guest maps to 0 must stay unemittable: BUTTON_C (98) and
    // BUTTON_Z (101) have no honored branch.
    assert(!morrow_input_guest_honors_key(98));
    assert(!morrow_input_guest_honors_key(101));
    // Out-of-range enum input is rejected.
    assert(morrow_input_map_android_key(MORROW_INPUT_BUTTON_COUNT) == -1);
    assert(morrow_input_map_android_axis(MORROW_INPUT_AXIS_COUNT) == -1);
    return 0;
}
