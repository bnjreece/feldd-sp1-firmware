#ifndef LOOP_CLOCK_H
#define LOOP_CLOCK_H
#include <stdint.h>

/* loop_clock.h — the control loop runs on a fixed cadence, not "work, then sleep".
 *
 * Pure; host-tested by firmware/test/test_loop_clock.c.
 *
 * WHY. The loop used to end with k_msleep(8): sleep 8 ms AFTER the work, so a
 * pass took work + 8 ms -- ~10 ms on an idle SP-1 (measured 2026-10-04), and
 * every timing counted in passes (debounce, LED blinks, the 625-pass •• power-off
 * hold) ran ~25% longer than written: the 5 s power-off took ~6 s. Under a heavy
 * load it stretched much further.
 *
 * NOW the loop sleeps until an absolute deadline one period after the previous
 * one. A short pass waits out the rest of the period, so idle passes are the
 * period exactly; a pass that overruns starts the next one at once, and the
 * schedule restarts from there -- it never runs a burst of back-to-back passes
 * to "catch up". */

/* The absolute wake time (ms, the uptime clock) for the pass after this one.
 * prev: the deadline this function last returned (0 before the first pass);
 * now: the uptime as the pass ends. */
static inline int64_t loop_clock_next(int64_t prev, int64_t now, int64_t period_ms)
{
    if (prev <= 0) {
        return now + period_ms;              /* first pass: one period from now */
    }
    int64_t next = prev + period_ms;
    if (next <= now) {
        return now;                          /* overran: go at once, no catch-up */
    }
    return next;
}

#endif /* LOOP_CLOCK_H */
