#include <assert.h>
#include <stdio.h>
#include "../src/gesture.h"

/* Host tests for the section-3 PLAY gesture FSM (spec
 * docs/superpowers/specs/2026-06-19-feldd-indicator-language-design.md §3).
 *
 * The gesture is stepped once per scan tick (~8 ms) with four signals:
 *   gesture_step(g, play_press, play_held, rocker_consumed_this_hold)
 *     play_press                 - 1 on the PLAY press-edge tick
 *     play_held                  - debounced hold (1 while PLAY is held down)
 *     rocker_consumed_this_hold  - 1 on the tick a FWD/RWD was consumed mid-hold
 *
 * Outputs: GESTURE_NONE, GESTURE_LAYER_STEP (double-tap), GESTURE_PEEK_PROFILE
 * (hold-alone, fires on RELEASE). Mode-switch is NOT an output here.
 *
 * Convenience: a PLAY press lasts (at least) one tick where both play_press=1
 * (the edge) AND play_held=1 (it is down). The tests model a press as one or
 * more held ticks, with play_press=1 only on the first. */

/* feed N fully-idle ticks (no press, not held, no rocker); none may fire */
static void idle(gesture_t *g, int n) {
    for (int i = 0; i < n; i++) {
        assert(gesture_step(g, 0, 0, 0) == GESTURE_NONE);
    }
}

/* feed N ticks of a sustained hold with no press-edge and no rocker; assert each
 * returns the expected code (held but not releasing -> usually GESTURE_NONE) */
static void hold(gesture_t *g, int n) {
    for (int i = 0; i < n; i++) {
        assert(gesture_step(g, 0, 1, 0) == GESTURE_NONE);
    }
}

/* --- 1. double-tap inside the window steps the layer, mod layer_count (2) --- */
static void test_double_tap_steps_layer_mod_2(void) {
    gesture_t g;
    gesture_init(&g);
    assert(gesture_layer(&g) == 0);

    /* first tap: press edge + held for that tick, then release. Arms, no step. */
    assert(gesture_step(&g, 1, 1, 0) == GESTURE_NONE);  /* press edge */
    assert(gesture_step(&g, 0, 0, 0) == GESTURE_NONE);  /* release */
    assert(gesture_layer(&g) == 0);

    idle(&g, 8); /* well inside the 45-tick window */

    /* second tap within the window: steps the layer 0 -> 1 */
    assert(gesture_step(&g, 1, 1, 0) == GESTURE_LAYER_STEP);
    assert(gesture_layer(&g) == 1);
    assert(gesture_step(&g, 0, 0, 0) == GESTURE_NONE);  /* release of 2nd tap */

    idle(&g, 5);

    /* a third double-tap wraps mod 2: 1 -> 0 */
    assert(gesture_step(&g, 1, 1, 0) == GESTURE_NONE);  /* arm */
    assert(gesture_step(&g, 0, 0, 0) == GESTURE_NONE);
    idle(&g, 5);
    assert(gesture_step(&g, 1, 1, 0) == GESTURE_LAYER_STEP);
    assert(gesture_layer(&g) == 0);
}

/* --- 2. hold past PEEK_HOLD_SCANS then release with no rocker -> PEEK on the
 *        RELEASE tick, never before --- */
static void test_hold_then_release_no_rocker_peeks_on_release(void) {
    gesture_t g;
    gesture_init(&g);

    /* press edge starts the hold */
    assert(gesture_step(&g, 1, 1, 0) == GESTURE_NONE);
    /* hold for PEEK_HOLD_SCANS-1 more ticks: hold_ticks reaches PEEK_HOLD_SCANS
     * on the arm tick but the peek must NOT fire while still held */
    for (int i = 0; i < PEEK_HOLD_SCANS - 1; i++) {
        assert(gesture_step(&g, 0, 1, 0) == GESTURE_NONE);
    }
    /* keep holding well past the arm point: still no fire while held */
    hold(&g, 20);

    /* RELEASE: now the armed peek commits */
    assert(gesture_step(&g, 0, 0, 0) == GESTURE_PEEK_PROFILE);
    /* layer untouched by a peek */
    assert(gesture_layer(&g) == 0);
}

/* a hold that releases BEFORE the peek arms must NOT peek */
static void test_short_hold_no_peek(void) {
    gesture_t g;
    gesture_init(&g);
    assert(gesture_step(&g, 1, 1, 0) == GESTURE_NONE);   /* press */
    hold(&g, PEEK_HOLD_SCANS - 10);                       /* not long enough */
    assert(gesture_step(&g, 0, 0, 0) == GESTURE_NONE);    /* release: no peek */
    assert(gesture_layer(&g) == 0);
}

/* --- 3. hold past the peek arm + a rocker_consumed mid-hold -> NO peek on
 *        release (rocker_seen voids it) and the layer is preserved --- */
static void test_hold_plus_rocker_voids_peek_preserves_layer(void) {
    gesture_t g;
    gesture_init(&g);
    /* engage layer 1 first via a double-tap, so we can prove it's preserved */
    assert(gesture_step(&g, 1, 1, 0) == GESTURE_NONE);
    assert(gesture_step(&g, 0, 0, 0) == GESTURE_NONE);
    idle(&g, 5);
    assert(gesture_step(&g, 1, 1, 0) == GESTURE_LAYER_STEP);
    assert(gesture_layer(&g) == 1);
    assert(gesture_step(&g, 0, 0, 0) == GESTURE_NONE);
    idle(&g, 10);

    /* now a long hold WITH a rocker consumed partway through */
    assert(gesture_step(&g, 1, 1, 0) == GESTURE_NONE);    /* press */
    hold(&g, 20);
    /* a FWD/RWD is consumed this tick: sets rocker_seen, voids any peek */
    assert(gesture_step(&g, 0, 1, 1) == GESTURE_NONE);
    /* keep holding well past PEEK_HOLD_SCANS: rocker_seen must keep peek voided */
    hold(&g, PEEK_HOLD_SCANS + 10);
    /* RELEASE: no peek (rocker_seen voided it) */
    assert(gesture_step(&g, 0, 0, 0) == GESTURE_NONE);
    /* layer preserved across the whole mode-switch hold */
    assert(gesture_layer(&g) == 1);
}

/* a rocker consumed AFTER the peek already armed must still void it on release */
static void test_rocker_after_arm_still_voids_peek(void) {
    gesture_t g;
    gesture_init(&g);
    assert(gesture_step(&g, 1, 1, 0) == GESTURE_NONE);    /* press */
    /* hold long enough to ARM the peek (peek_pending=1) */
    hold(&g, PEEK_HOLD_SCANS + 5);
    /* THEN a rocker arrives: must clear peek_pending + set rocker_seen */
    assert(gesture_step(&g, 0, 1, 1) == GESTURE_NONE);
    hold(&g, 5);
    /* RELEASE: voided, no peek */
    assert(gesture_step(&g, 0, 0, 0) == GESTURE_NONE);
}

/* --- 4. a slow double-tap whose inter-press gap never reaches PEEK_HOLD_SCANS
 *        returns LAYER_STEP and never a spurious peek --- */
static void test_slow_double_tap_no_spurious_peek(void) {
    gesture_t g;
    gesture_init(&g);
    /* first tap (one held tick + release) */
    assert(gesture_step(&g, 1, 1, 0) == GESTURE_NONE);
    assert(gesture_step(&g, 0, 0, 0) == GESTURE_NONE);
    /* a gap that is long-ish (between the two taps PLAY is RELEASED, so hold_ticks
     * never climbs toward 75) but still inside DOUBLE_TAP_WINDOW_SCANS (45) */
    idle(&g, DOUBLE_TAP_WINDOW_SCANS - 5);
    /* second tap inside the window -> LAYER_STEP, and absolutely no peek anywhere */
    assert(gesture_step(&g, 1, 1, 0) == GESTURE_LAYER_STEP);
    assert(gesture_layer(&g) == 1);
    /* releasing the 2nd tap must NOT fire a peek (it was a tap, not a hold) */
    assert(gesture_step(&g, 0, 0, 0) == GESTURE_NONE);
}

/* a single lone tap that is never followed within the window does nothing */
static void test_single_tap_does_nothing(void) {
    gesture_t g;
    gesture_init(&g);
    assert(gesture_step(&g, 1, 1, 0) == GESTURE_NONE);   /* lone tap */
    assert(gesture_step(&g, 0, 0, 0) == GESTURE_NONE);   /* release */
    idle(&g, DOUBLE_TAP_WINDOW_SCANS + 5);               /* window expires */
    assert(gesture_layer(&g) == 0);
    /* a press now is a NEW first tap, not a double with the expired one */
    assert(gesture_step(&g, 1, 1, 0) == GESTURE_NONE);
    assert(gesture_layer(&g) == 0);
}

/* a second tap just past the window is treated as a new first tap, not a double */
static void test_second_tap_just_outside_window_is_new_first_tap(void) {
    gesture_t g;
    gesture_init(&g);
    assert(gesture_step(&g, 1, 1, 0) == GESTURE_NONE);   /* first tap */
    assert(gesture_step(&g, 0, 0, 0) == GESTURE_NONE);   /* release */
    idle(&g, DOUBLE_TAP_WINDOW_SCANS + 1);               /* just past the window */
    assert(gesture_step(&g, 1, 1, 0) == GESTURE_NONE);   /* too late -> no step */
    assert(gesture_layer(&g) == 0);
}

/* --- 5. gesture_disarm clears the arm but preserves an engaged layer --- */
static void test_disarm_clears_arm_preserves_layer(void) {
    gesture_t g;
    gesture_init(&g);

    /* Arm a first tap, then disarm: the very next press must be a NEW first tap
     * (no step), proving the pending arm was cleared. */
    assert(gesture_step(&g, 1, 1, 0) == GESTURE_NONE);   /* arm */
    assert(gesture_step(&g, 0, 0, 0) == GESTURE_NONE);   /* release */
    gesture_disarm(&g);
    assert(gesture_step(&g, 1, 1, 0) == GESTURE_NONE);   /* new first tap, no step */
    assert(gesture_layer(&g) == 0);

    /* Step the layer to 1, then disarm: the engaged layer must SURVIVE. */
    assert(gesture_step(&g, 0, 0, 0) == GESTURE_NONE);
    idle(&g, 5);
    assert(gesture_step(&g, 1, 1, 0) == GESTURE_LAYER_STEP);  /* 2nd tap -> layer 1 */
    assert(gesture_layer(&g) == 1);
    assert(gesture_step(&g, 0, 0, 0) == GESTURE_NONE);
    gesture_disarm(&g);
    assert(gesture_layer(&g) == 1);                      /* layer preserved */
}

/* the gesture_shift() / gesture_latched() accessor (back-compat for main.c's
 * shift bank read) tracks the layer's low bit so a 2-layer build still reads a
 * 0/1 shift flag */
static void test_shift_accessor_tracks_layer(void) {
    gesture_t g;
    gesture_init(&g);
    assert(gesture_shift(&g) == 0);
    assert(gesture_step(&g, 1, 1, 0) == GESTURE_NONE);   /* arm */
    assert(gesture_step(&g, 0, 0, 0) == GESTURE_NONE);
    idle(&g, 5);
    assert(gesture_step(&g, 1, 1, 0) == GESTURE_LAYER_STEP);
    assert(gesture_shift(&g) == 1);                      /* layer 1 -> shift on */
}

int main(void) {
    test_double_tap_steps_layer_mod_2();
    test_hold_then_release_no_rocker_peeks_on_release();
    test_short_hold_no_peek();
    test_hold_plus_rocker_voids_peek_preserves_layer();
    test_rocker_after_arm_still_voids_peek();
    test_slow_double_tap_no_spurious_peek();
    test_single_tap_does_nothing();
    test_second_tap_just_outside_window_is_new_first_tap();
    test_disarm_clears_arm_preserves_layer();
    test_shift_accessor_tracks_layer();
    printf("all gesture tests passed\n");
    return 0;
}
