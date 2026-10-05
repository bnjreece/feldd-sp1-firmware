#include <assert.h>
#include <stdio.h>
#include "flash_defer.h"

/* Nothing owed: nothing to take, whatever the state. */
static void t_empty_takes_nothing(void)
{
    struct flash_defer d;
    flash_defer_init(&d);
    assert(!flash_defer_pending(&d));
    assert(flash_defer_take(&d, 0, true, false) == 0u);
    assert(flash_defer_take(&d, 0, false, true) == 0u);
}

/* The core rule: a change made while NOT quiet waits, and goes at the first
 * quiet pass -- once. */
static void t_waits_for_quiet_then_writes_once(void)
{
    struct flash_defer d;
    flash_defer_init(&d);
    flash_defer_mark(&d, FLASH_DEFER_HEADER);
    assert(flash_defer_pending(&d));
    assert(flash_defer_take(&d, 10, false, false) == 0u);   /* sounding: hold */
    assert(flash_defer_take(&d, 20, false, false) == 0u);
    assert(flash_defer_pending(&d));
    assert(flash_defer_take(&d, 30, true, false) == FLASH_DEFER_HEADER);
    assert(!flash_defer_pending(&d));
    assert(flash_defer_take(&d, 40, true, false) == 0u);    /* not twice */
}

/* Many changes while sounding coalesce into one write per record: stepping
 * through five profiles during a held chord costs one header write, not five. */
static void t_repeated_marks_coalesce(void)
{
    struct flash_defer d;
    flash_defer_init(&d);
    for (int i = 0; i < 5; i++) {
        flash_defer_mark(&d, FLASH_DEFER_HEADER);
    }
    flash_defer_mark(&d, FLASH_DEFER_SETTINGS);
    assert(flash_defer_take(&d, 0, true, false) == (FLASH_DEFER_HEADER | FLASH_DEFER_SETTINGS));
    assert(!flash_defer_pending(&d));
}

/* Power-off writes whatever is owed even while sounding. */
static void t_force_ignores_quiet(void)
{
    struct flash_defer d;
    flash_defer_init(&d);
    flash_defer_mark(&d, FLASH_DEFER_SETTINGS);
    assert(flash_defer_take(&d, 0, false, true) == FLASH_DEFER_SETTINGS);
    assert(!flash_defer_pending(&d));
}

/* A failure is owed again, held off for the retry window, then retried. */
static void t_failure_backs_off_then_retries(void)
{
    struct flash_defer d;
    flash_defer_init(&d);
    flash_defer_mark(&d, FLASH_DEFER_HEADER);
    uint8_t r = flash_defer_take(&d, 100, true, false);
    assert(r == FLASH_DEFER_HEADER);
    flash_defer_failed(&d, r, 100);
    assert(flash_defer_pending(&d));
    assert(flash_defer_take(&d, 100, true, false) == 0u);
    assert(flash_defer_take(&d, 100 + FLASH_DEFER_RETRY_MS - 1u, true, false) == 0u);
    assert(flash_defer_take(&d, 100 + FLASH_DEFER_RETRY_MS, true, false) == FLASH_DEFER_HEADER);
    /* a success clears the backoff: the next change goes at once */
    flash_defer_mark(&d, FLASH_DEFER_HEADER);
    assert(flash_defer_take(&d, 100 + FLASH_DEFER_RETRY_MS, true, false) == FLASH_DEFER_HEADER);
}

/* A record changed during the backoff waits with the failed one. */
static void t_backoff_holds_new_marks_too(void)
{
    struct flash_defer d;
    flash_defer_init(&d);
    flash_defer_mark(&d, FLASH_DEFER_HEADER);
    flash_defer_failed(&d, flash_defer_take(&d, 0, true, false), 0);
    flash_defer_mark(&d, FLASH_DEFER_SETTINGS);
    assert(flash_defer_take(&d, 500, true, false) == 0u);
    assert(flash_defer_take(&d, FLASH_DEFER_RETRY_MS, true, false)
           == (FLASH_DEFER_HEADER | FLASH_DEFER_SETTINGS));
}

/* Power-off does not wait out a backoff: it is the last chance to write. */
static void t_force_ignores_backoff(void)
{
    struct flash_defer d;
    flash_defer_init(&d);
    flash_defer_mark(&d, FLASH_DEFER_HEADER);
    flash_defer_failed(&d, flash_defer_take(&d, 0, true, false), 0);
    assert(flash_defer_take(&d, 1, false, true) == FLASH_DEFER_HEADER);
}

/* The backoff compares wrap-safely: uptime in ms wraps after ~49 days. */
static void t_backoff_is_wrap_safe(void)
{
    struct flash_defer d;
    flash_defer_init(&d);
    const uint32_t near_wrap = 0xFFFFFF00u;
    flash_defer_mark(&d, FLASH_DEFER_HEADER);
    flash_defer_failed(&d, flash_defer_take(&d, near_wrap, true, false), near_wrap);
    assert(flash_defer_take(&d, near_wrap + 10u, true, false) == 0u);
    assert(flash_defer_take(&d, near_wrap + FLASH_DEFER_RETRY_MS, true, false) == FLASH_DEFER_HEADER);
}

/* A record written by another path is no longer owed; the other one still is. */
static void t_clear_drops_only_that_record(void)
{
    struct flash_defer d;
    flash_defer_init(&d);
    flash_defer_mark(&d, FLASH_DEFER_HEADER | FLASH_DEFER_SETTINGS);
    flash_defer_clear(&d, FLASH_DEFER_SETTINGS);
    assert(flash_defer_take(&d, 0, true, false) == FLASH_DEFER_HEADER);
}

int main(void)
{
    t_empty_takes_nothing();
    t_waits_for_quiet_then_writes_once();
    t_repeated_marks_coalesce();
    t_force_ignores_quiet();
    t_failure_backs_off_then_retries();
    t_backoff_holds_new_marks_too();
    t_force_ignores_backoff();
    t_backoff_is_wrap_safe();
    t_clear_drops_only_that_record();
    printf("test_flash_defer: OK\n");
    return 0;
}
