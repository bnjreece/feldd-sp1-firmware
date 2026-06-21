#include <assert.h>
#include <stdio.h>
#include "../src/gesture.h"

/* Host tests for the PLAY gesture FSM after the #61 gesture pass (spec
 * notes/2026-06-20-feldd-gesture-pass-spec.md §2/§3).
 *
 * The gesture is stepped once per scan tick (~8 ms) with two signals now (the
 * old press-edge arg is GONE — layer select moved to main.c's layer count-dial):
 *   gesture_step(g, play_held, rocker_consumed_this_hold)
 *     play_held                  - debounced hold (1 while PLAY is held down)
 *     rocker_consumed_this_hold  - 1 on the tick a FWD/RWD was consumed mid-hold
 *
 * Outputs: GESTURE_NONE, GESTURE_PEEK_PROFILE (hold-alone, fires on RELEASE).
 * GESTURE_LAYER_STEP is REMOVED (the double-tap layer round-robin is gone; the
 * layer is now SET via gesture_set_layer by main.c's dial commit). Mode-switch is
 * NOT an output here.
 *
 * Convenience: a PLAY hold is modeled as one or more play_held=1 ticks followed
 * by a release (play_held=0). */

/* feed N fully-idle ticks (not held, no rocker); none may fire */
static void idle(gesture_t *g, int n) {
    for (int i = 0; i < n; i++) {
        assert(gesture_step(g, 0, 0) == GESTURE_NONE);
    }
}

/* feed N ticks of a sustained hold with no rocker; assert each returns
 * GESTURE_NONE (held but not releasing) */
static void hold(gesture_t *g, int n) {
    for (int i = 0; i < n; i++) {
        assert(gesture_step(g, 1, 0) == GESTURE_NONE);
    }
}

/* --- 1. gesture_set_layer engages the layer (the layer is now SET by main.c's
 *        layer count-dial, not stepped by a double-tap). Clamps into range. --- */
static void test_set_layer_engages_and_clamps(void) {
    gesture_t g;
    gesture_init(&g);
    assert(gesture_layer(&g) == 0);

    gesture_set_layer(&g, 1);
    assert(gesture_layer(&g) == 1);

    gesture_set_layer(&g, 3);
    assert(gesture_layer(&g) == 3);

    gesture_set_layer(&g, 0);
    assert(gesture_layer(&g) == 0);

    /* over-range clamps to the top layer (GESTURE_LAYER_COUNT-1 = 3); negative
     * clamps to 0. main.c clamps first, but the setter guards independently. */
    gesture_set_layer(&g, 7);
    assert(gesture_layer(&g) == GESTURE_LAYER_COUNT - 1);
    assert(gesture_layer(&g) == 3);
    gesture_set_layer(&g, -2);
    assert(gesture_layer(&g) == 0);
}

/* --- 2. hold past PEEK_HOLD_SCANS then release with no rocker -> PEEK on the
 *        RELEASE tick, never before --- */
static void test_hold_then_release_no_rocker_peeks_on_release(void) {
    gesture_t g;
    gesture_init(&g);

    /* press edge starts the hold */
    assert(gesture_step(&g, 1, 0) == GESTURE_NONE);
    /* hold for PEEK_HOLD_SCANS-1 more ticks: hold_ticks reaches PEEK_HOLD_SCANS
     * on the arm tick but the peek must NOT fire while still held */
    for (int i = 0; i < PEEK_HOLD_SCANS - 1; i++) {
        assert(gesture_step(&g, 1, 0) == GESTURE_NONE);
    }
    /* keep holding well past the arm point: still no fire while held */
    hold(&g, 20);

    /* RELEASE: now the armed peek commits */
    assert(gesture_step(&g, 0, 0) == GESTURE_PEEK_PROFILE);
    /* layer untouched by a peek */
    assert(gesture_layer(&g) == 0);
}

/* a hold that releases BEFORE the peek arms must NOT peek */
static void test_short_hold_no_peek(void) {
    gesture_t g;
    gesture_init(&g);
    assert(gesture_step(&g, 1, 0) == GESTURE_NONE);   /* press */
    hold(&g, PEEK_HOLD_SCANS - 10);                    /* not long enough */
    assert(gesture_step(&g, 0, 0) == GESTURE_NONE);    /* release: no peek */
    assert(gesture_layer(&g) == 0);
}

/* a PLAY TAP (a short hold of 1..40 ticks, the layer-dial tap band in main.c)
 * must NOT arm a peek — the tap band (<=40) is disjoint from the peek arm (75).
 * gesture.c sees only the debounced hold, so model the longest possible dial tap
 * (40 held ticks) and a single-tick tap, and prove neither peeks on release. */
static void test_dial_tap_does_not_arm_peek(void) {
    gesture_t g;
    gesture_init(&g);

    /* a 40-tick hold (the upper edge of the layer-dial tap band) then release */
    assert(gesture_step(&g, 1, 0) == GESTURE_NONE);
    hold(&g, 39);                                      /* 40 held ticks total */
    assert(gesture_step(&g, 0, 0) == GESTURE_NONE);    /* release: NO peek */
    assert(gesture_layer(&g) == 0);

    idle(&g, 10);

    /* a single-tick tap then release: also no peek */
    assert(gesture_step(&g, 1, 0) == GESTURE_NONE);
    assert(gesture_step(&g, 0, 0) == GESTURE_NONE);    /* release: NO peek */
    assert(gesture_layer(&g) == 0);
}

/* --- 3. hold past the peek arm + a rocker_consumed mid-hold -> NO peek on
 *        release (rocker_seen voids it) and the engaged layer is preserved --- */
static void test_hold_plus_rocker_voids_peek_preserves_layer(void) {
    gesture_t g;
    gesture_init(&g);
    /* engage layer 1 first (as main.c's dial would), so we can prove it's kept */
    gesture_set_layer(&g, 1);
    assert(gesture_layer(&g) == 1);

    /* now a long hold WITH a rocker consumed partway through */
    assert(gesture_step(&g, 1, 0) == GESTURE_NONE);    /* press */
    hold(&g, 20);
    /* a FWD/RWD is consumed this tick: sets rocker_seen, voids any peek */
    assert(gesture_step(&g, 1, 1) == GESTURE_NONE);
    /* keep holding well past PEEK_HOLD_SCANS: rocker_seen must keep peek voided */
    hold(&g, PEEK_HOLD_SCANS + 10);
    /* RELEASE: no peek (rocker_seen voided it) */
    assert(gesture_step(&g, 0, 0) == GESTURE_NONE);
    /* layer preserved across the whole mode-switch hold */
    assert(gesture_layer(&g) == 1);
}

/* a rocker consumed AFTER the peek already armed must still void it on release */
static void test_rocker_after_arm_still_voids_peek(void) {
    gesture_t g;
    gesture_init(&g);
    assert(gesture_step(&g, 1, 0) == GESTURE_NONE);    /* press */
    /* hold long enough to ARM the peek (peek_pending=1) */
    hold(&g, PEEK_HOLD_SCANS + 5);
    /* THEN a rocker arrives: must clear peek_pending + set rocker_seen */
    assert(gesture_step(&g, 1, 1) == GESTURE_NONE);
    hold(&g, 5);
    /* RELEASE: voided, no peek */
    assert(gesture_step(&g, 0, 0) == GESTURE_NONE);
}

/* --- 4. gesture_disarm clears a pending peek but preserves an engaged layer --- */
static void test_disarm_clears_peek_preserves_layer(void) {
    gesture_t g;
    gesture_init(&g);

    /* engage layer 1, then arm a peek, then disarm: the disarm must clear the
     * pending peek (so a release does NOT fire) yet keep the layer. */
    gesture_set_layer(&g, 1);
    assert(gesture_step(&g, 1, 0) == GESTURE_NONE);    /* press */
    hold(&g, PEEK_HOLD_SCANS + 5);                     /* arm the peek */
    gesture_disarm(&g);                                /* clear the pending peek */
    assert(gesture_step(&g, 0, 0) == GESTURE_NONE);    /* release: NO peek now */
    assert(gesture_layer(&g) == 1);                    /* layer preserved */
}

/* the gesture_shift() / gesture_latched() accessor (back-compat for main.c's
 * shift bank read) tracks the engaged layer's low bit so a 2-layer build still
 * reads a 0/1 shift flag */
static void test_shift_accessor_tracks_layer(void) {
    gesture_t g;
    gesture_init(&g);
    assert(gesture_shift(&g) == 0);
    gesture_set_layer(&g, 1);
    assert(gesture_shift(&g) == 1);                    /* layer 1 -> shift on */
    assert(gesture_latched(&g) == 1);
    gesture_set_layer(&g, 2);
    assert(gesture_shift(&g) == 0);                    /* layer 2 low bit = 0 */
    gesture_set_layer(&g, 3);
    assert(gesture_shift(&g) == 1);                    /* layer 3 low bit = 1 */
}

int main(void) {
    test_set_layer_engages_and_clamps();
    test_hold_then_release_no_rocker_peeks_on_release();
    test_short_hold_no_peek();
    test_dial_tap_does_not_arm_peek();
    test_hold_plus_rocker_voids_peek_preserves_layer();
    test_rocker_after_arm_still_voids_peek();
    test_disarm_clears_peek_preserves_layer();
    test_shift_accessor_tracks_layer();
    printf("all gesture tests passed\n");
    return 0;
}
