#include <assert.h>
#include <stdio.h>
#include "loop_clock.h"

/* Idle: the work is short, so the deadlines are exactly one period apart and
 * do not drift with how long each pass took. */
static void t_steady_cadence(void)
{
    int64_t d = loop_clock_next(0, 1000, 8);
    assert(d == 1008);
    d = loop_clock_next(d, 1010, 8);         /* pass took 2 ms: still on schedule */
    assert(d == 1016);
    d = loop_clock_next(d, 1017, 8);         /* 1 ms of work */
    assert(d == 1024);
}

/* An overrun starts the next pass at once and restarts the schedule from there:
 * no burst of back-to-back passes to catch up. */
static void t_overrun_no_catch_up(void)
{
    int64_t d = loop_clock_next(0, 0, 8);    /* 8 */
    d = loop_clock_next(d, 30, 8);           /* this pass ran to 30: overrun */
    assert(d == 30);                          /* wake now */
    d = loop_clock_next(d, 31, 8);           /* short pass: one period after 30 */
    assert(d == 38);
}

/* A pass ending exactly on the next deadline counts as an overrun (go now). */
static void t_exact_boundary(void)
{
    assert(loop_clock_next(8, 16, 8) == 16);
}

/* The 625-pass power-off hold is 5000 ms of deadlines at an 8 ms period. */
static void t_625_passes_is_5_s(void)
{
    int64_t d = loop_clock_next(0, 0, 8), start = 0;
    for (int i = 1; i < 625; i++) d = loop_clock_next(d, d + 1, 8);
    assert(d - start == 5000);
}

int main(void)
{
    t_steady_cadence();
    t_overrun_no_catch_up();
    t_exact_boundary();
    t_625_passes_is_5_s();
    printf("test_loop_clock: OK\n");
    return 0;
}
