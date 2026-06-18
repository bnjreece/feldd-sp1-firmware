#include <assert.h>
#include <stdio.h>
#include "../src/gesture.h"

/* feed N scan ticks with no PLAY press; none of them may toggle the latch */
static void idle(gesture_t *g, int n) {
    for (int i = 0; i < n; i++) {
        assert(gesture_step(g, 0) == GESTURE_NONE);
    }
}

/* a double-tap inside the window toggles the latch; a second double-tap clears it */
static void test_double_tap_within_window_toggles_latch(void) {
    gesture_t g;
    gesture_init(&g);
    assert(gesture_latched(&g) == 0);

    /* first tap: arms, no toggle yet */
    assert(gesture_step(&g, 1) == GESTURE_NONE);
    assert(gesture_latched(&g) == 0);
    idle(&g, 10); /* still well inside the window */

    /* second tap within the window: toggles ON */
    assert(gesture_step(&g, 1) == GESTURE_LATCH_TOGGLED);
    assert(gesture_latched(&g) == 1);

    /* a fresh double-tap toggles it back OFF */
    assert(gesture_step(&g, 1) == GESTURE_NONE);
    idle(&g, 5);
    assert(gesture_step(&g, 1) == GESTURE_LATCH_TOGGLED);
    assert(gesture_latched(&g) == 0);
}

/* a lone tap that is never followed within the window does nothing */
static void test_single_tap_does_not_toggle(void) {
    gesture_t g;
    gesture_init(&g);
    assert(gesture_step(&g, 1) == GESTURE_NONE);   /* lone first tap */
    idle(&g, DOUBLE_TAP_WINDOW_SCANS + 5);         /* window expires */
    assert(gesture_latched(&g) == 0);
    /* a press now is a NEW first tap, not a double with the expired one */
    assert(gesture_step(&g, 1) == GESTURE_NONE);
    assert(gesture_latched(&g) == 0);
}

/* a second tap just past the window is treated as a new first tap, not a double */
static void test_second_tap_just_outside_window_is_new_first_tap(void) {
    gesture_t g;
    gesture_init(&g);
    assert(gesture_step(&g, 1) == GESTURE_NONE);       /* first tap */
    idle(&g, DOUBLE_TAP_WINDOW_SCANS + 1);             /* just past the window */
    assert(gesture_step(&g, 1) == GESTURE_NONE);       /* too late -> no toggle */
    assert(gesture_latched(&g) == 0);
}

/* gesture_latched() is the exact value wired into map_fader/map_button's shift
 * arg; prove it tracks the toggles so the firmware drives the right bank */
static void test_latched_query_tracks_toggles(void) {
    gesture_t g;
    gesture_init(&g);
    assert(gesture_step(&g, 1) == GESTURE_NONE);          /* arm */
    assert(gesture_step(&g, 1) == GESTURE_LATCH_TOGGLED); /* immediate second press */
    assert(gesture_latched(&g) == 1);
}

int main(void) {
    test_double_tap_within_window_toggles_latch();
    test_single_tap_does_not_toggle();
    test_second_tap_just_outside_window_is_new_first_tap();
    test_latched_query_tracks_toggles();
    printf("all gesture tests passed\n");
    return 0;
}
