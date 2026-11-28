#include "ui_navigation.h"

#include <assert.h>
#include <stdio.h>

int main(void)
{
    unsigned page = 999;
    const unsigned count = 8;
    for (unsigned row = 0; row < count; row++) {
        const unsigned y = MORROW_RAIL_TOP + row * MORROW_RAIL_STEP;
        assert(morrow_rail_hit(0, y, count, &page) && page == row);
        assert(morrow_rail_hit(MORROW_RAIL_X_LIMIT - 1,
                               y + MORROW_RAIL_ROW_HEIGHT - 1,
                               count, &page) && page == row);
        assert(!morrow_rail_hit(0, y + MORROW_RAIL_ROW_HEIGHT, count, &page));
    }
    assert(!morrow_rail_hit(0, MORROW_RAIL_TOP - 1, count, &page));
    assert(!morrow_rail_hit(MORROW_RAIL_X_LIMIT, MORROW_RAIL_TOP, count, &page));
    assert(!morrow_rail_hit(0, MORROW_RAIL_TOP + count * MORROW_RAIL_STEP,
                            count, &page));
    assert(!morrow_rail_hit(0, MORROW_RAIL_TOP, 0, &page));
    assert(morrow_rail_hit(0, MORROW_RAIL_TOP, count, NULL));
    puts("Morrow sidebar navigation passed");
    return 0;
}
