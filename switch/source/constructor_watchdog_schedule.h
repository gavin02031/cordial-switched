#ifndef MORROW_CONSTRUCTOR_WATCHDOG_SCHEDULE_H
#define MORROW_CONSTRUCTOR_WATCHDOG_SCHEDULE_H

#include <stdbool.h>
#include <limits.h>

// A constructor can fault before the previous three-second watchdog sample.
// Observe briefly at 20/100/300 ms, then back off. No guest work is skipped.
static inline unsigned morrow_watchdog_delay_ms(unsigned elapsed_ms)
{
    if (elapsed_ms < 100) return 20;
    if (elapsed_ms < 1000) return 50;
    return 1000;
}

// Treat the first observation of a new active constructor as a sample tick.
// In particular, do not discard a 20 ms observation just because the
// constructor changed since the previous poll. Saturate on counter overflow.
static inline unsigned morrow_watchdog_advance_elapsed(
    unsigned elapsed_ms, unsigned delay_ms, bool new_constructor)
{
    if (new_constructor) return delay_ms;
    return elapsed_ms > UINT_MAX - delay_ms ? UINT_MAX
                                           : elapsed_ms + delay_ms;
}

static inline bool morrow_watchdog_sample_due(unsigned elapsed_ms)
{
    return elapsed_ms == 20 || elapsed_ms == 100 ||
           elapsed_ms == 300 || elapsed_ms == 1000 ||
           elapsed_ms == 3000 || elapsed_ms == 10000 ||
           (elapsed_ms >= 30000 && elapsed_ms % 30000 == 0);
}

#endif
