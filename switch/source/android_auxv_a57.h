#ifndef MORROW_ANDROID_AUXV_A57_H
#define MORROW_ANDROID_AUXV_A57_H

#include <stdbool.h>

// Android/Linux AArch64 AT_HWCAP=16, AT_HWCAP2=26. Horizon has no
// Linux auxv. Cortex-A57 supports baseline FP and ASIMD (NEON), but
// does not implement ARMv8.1 LSE atomic instructions (HWCAP bit 8).
static inline unsigned long morrow_a57_auxv_value(unsigned long key,
                                                   bool *known)
{
    if (known) *known = true;
    if (key == 16) return (1UL << 0) | (1UL << 1);
    if (key == 26) return 0;
    if (known) *known = false;
    return 0;
}
#endif
