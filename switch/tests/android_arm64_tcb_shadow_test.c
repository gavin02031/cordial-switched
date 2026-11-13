#include "android_arm64_tcb_shadow.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    struct {
        uint64_t before;
        AndroidArm64TcbShadow value;
        uint64_t after;
    } frame;
    memset(&frame, 0xa5, sizeof(frame));
    frame.before = UINT64_C(0x1234567890abcdef);
    frame.after = UINT64_C(0xfedcba0987654321);

    void *identity = (void *)(uintptr_t)0x12345670;
    android_arm64_tcb_shadow_init(&frame.value, identity);
    const void *const *tp = android_arm64_tcb_shadow_pointer(&frame.value);
    assert(tp);
    assert(tp[-1] == NULL);
    assert(tp[0] == NULL);
    assert(tp[1] == identity);
    for (int slot = 2; slot <= 7; slot++)
        assert(tp[slot] == NULL);
    assert((const unsigned char *)tp == (const unsigned char *)&frame.value + 8);
    assert(frame.before == UINT64_C(0x1234567890abcdef));
    assert(frame.after == UINT64_C(0xfedcba0987654321));

    assert(android_arm64_tcb_shadow_pointer(NULL) == NULL);
    android_arm64_tcb_shadow_init(NULL, identity);
    puts("android_arm64_tcb_shadow_test: ARM64 slots -1..7 and canaries passed");
    return 0;
}
