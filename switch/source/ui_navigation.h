#ifndef MORROW_UI_NAVIGATION_H
#define MORROW_UI_NAVIGATION_H

#include <stdbool.h>

// Keep the SDL rail and Horizon touchscreen hit targets in lockstep. The
// seven rows must leave the lower status card unobstructed at y=571.
enum {
    MORROW_RAIL_TOP = 185,
    MORROW_RAIL_STEP = 51,
    MORROW_RAIL_ROW_HEIGHT = 45,
    MORROW_RAIL_X_LIMIT = 273
};

static inline bool morrow_rail_hit(unsigned x, unsigned y,
                                   unsigned page_count, unsigned *page)
{
    if (x >= MORROW_RAIL_X_LIMIT || y < MORROW_RAIL_TOP || !page_count)
        return false;
    const unsigned relative = y - MORROW_RAIL_TOP;
    const unsigned index = relative / MORROW_RAIL_STEP;
    if (index >= page_count || relative % MORROW_RAIL_STEP >= MORROW_RAIL_ROW_HEIGHT)
        return false;
    if (page) *page = index;
    return true;
}

#endif
