#include "android_auxv_a57.h"
#include <assert.h>
#include <stdio.h>

int main(void)
{
    bool known = false;
    unsigned long caps = morrow_a57_auxv_value(16, &known);
    assert(known && caps == 3UL);
    assert((caps & (1UL << 8)) == 0); // Must not advertise LSE atomics.
    assert(morrow_a57_auxv_value(26, &known) == 0 && known);
    assert(morrow_a57_auxv_value(100, &known) == 0 && !known);
    puts("ARM64 Cortex-A57 guest auxiliary vector: pass");
    return 0;
}
