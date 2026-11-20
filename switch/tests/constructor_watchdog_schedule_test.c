#include "constructor_watchdog_schedule.h"

#include <assert.h>
#include <stdio.h>

int main(void)
{
    assert(morrow_watchdog_delay_ms(0) == 20);
    assert(morrow_watchdog_delay_ms(80) == 20);
    assert(morrow_watchdog_delay_ms(100) == 50);
    assert(morrow_watchdog_delay_ms(950) == 50);
    assert(morrow_watchdog_delay_ms(1000) == 1000);

    unsigned elapsed = 0;
    unsigned found = 0;
    const unsigned expected[] = {20, 100, 300, 1000, 3000,
                                 10000, 30000, 60000};
    while (elapsed < 60000) {
        elapsed += morrow_watchdog_delay_ms(elapsed);
        if (morrow_watchdog_sample_due(elapsed)) {
            assert(found < sizeof(expected) / sizeof(expected[0]));
            assert(elapsed == expected[found++]);
        }
    }
    assert(found == sizeof(expected) / sizeof(expected[0]));
    puts("Short-interval constructor watchdog schedule: pass");
    return 0;
}
