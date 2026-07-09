#include <assert.h>
#include <stdio.h>
#include "../src/dial.h"
#include "../src/gesture.h"

/* Feed N scan ticks with NO tap; assert each one is a quiet DIAL_NONE. Returns
 * the LAST result so the caller can also inspect the tick a commit fires (a
 * commit lands on an idle tick, not on a tap tick). */
static dial_result_t idle_quiet(dial_t *d, int n) {
    dial_result_t r;
    r.kind = DIAL_NONE;
    r.count = 0;
    for (int i = 0; i < n; i++) {
        r = dial_step(d, 0);
        assert(r.kind == DIAL_NONE);
    }
    return r;
}

/* Single tap edge on one tick, then nothing. */
static dial_result_t tap(dial_t *d) {
    return dial_step(d, 1);
}

/* Idle until a non-NONE result fires (or fail after a generous bound). Every
 * tick before the commit must be DIAL_NONE. Used to ride out the inter-tap +
 * commit windows and catch whatever the burst resolves to. */
static dial_result_t idle_quiet_until_commit(dial_t *d) {
    for (int i = 0; i < 200; i++) {
        dial_result_t r = dial_step(d, 0);
        if (r.kind != DIAL_NONE) return r;
    }
    assert(0 && "no commit fired");
    dial_result_t none = { DIAL_NONE, 0 };
    return none;
}

/* ----------------------------------------------------------------------------
 * 1. A lone tap -> RELATIVE_NEXT. The single relative tap pays NO absolute
 *    COMMIT_WINDOW (its commit IS the inter-tap-window expiry, not the longer
 *    multi-tap commit tail). It does, however, wait out INTER_TAP_WINDOW so a 2nd
 *    tap can still upgrade the burst to ABSOLUTE — see the timing comment in
 *    dial.c (correctness of the count beats the relative micro-latency). So:
 *      - on the tap edge itself: still NONE (armed, could upgrade)
 *      - within the inter-tap window: still NONE
 *      - the tick the inter-tap window expires: RELATIVE_NEXT fires
 *      - it NEVER waits the extra COMMIT_WINDOW the absolute path pays.
 * --------------------------------------------------------------------------*/
static void test_lone_tap_is_relative_next(void) {
    dial_t d;
    dial_init(&d);

    dial_result_t r = tap(&d);            /* tap-release edge: armed, not classified */
    assert(r.kind == DIAL_NONE);

    /* still nothing right up to the inter-tap window boundary */
    idle_quiet(&d, INTER_TAP_WINDOW);     /* ticks 1..INTER_TAP_WINDOW after the tap */

    /* the NEXT idle tick (since_tap > INTER_TAP_WINDOW) resolves the lone tap */
    dial_result_t rel = dial_step(&d, 0);
    assert(rel.kind == DIAL_RELATIVE_NEXT);

    /* and it did NOT pay the absolute commit tail: it fired at the inter-tap
     * boundary, strictly before INTER_TAP_WINDOW + COMMIT_WINDOW would elapse */

    /* nothing further fires while idling out */
    idle_quiet(&d, COMMIT_WINDOW + 5);
}

/* ----------------------------------------------------------------------------
 * 2. Two fast taps within INTER_TAP_WINDOW -> ABSOLUTE(2), committed only after
 *    COMMIT_WINDOW idle ticks (NOT on the 2nd tap tick).
 * --------------------------------------------------------------------------*/
static void test_two_fast_taps_absolute_two(void) {
    dial_t d;
    dial_init(&d);

    /* First tap must NOT immediately fire RELATIVE — a 2nd tap can still upgrade
     * the burst to absolute. The single relative result is held until the FSM
     * knows the burst was lone, i.e. until INTER_TAP_WINDOW expires with count 1.
     * (See the design comment in dial.c for the instant-vs-upgrade tradeoff.) */
    dial_result_t r1 = tap(&d);
    assert(r1.kind == DIAL_NONE);   /* armed, not yet classified */

    idle_quiet(&d, 5);              /* well inside INTER_TAP_WINDOW */

    dial_result_t r2 = tap(&d);     /* 2nd tap -> burst is now multi-tap */
    assert(r2.kind == DIAL_NONE);   /* absolute waits for the commit window */

    /* commit fires after COMMIT_WINDOW idle ticks, carrying count 2 */
    dial_result_t c = idle_quiet_until_commit(&d);
    assert(c.kind == DIAL_ABSOLUTE);
    assert(c.count == 2);
}

/* ----------------------------------------------------------------------------
 * 3. Eight fast taps -> ABSOLUTE(8).
 * --------------------------------------------------------------------------*/
static void test_eight_fast_taps_absolute_eight(void) {
    dial_t d;
    dial_init(&d);

    tap(&d);
    for (int i = 0; i < 7; i++) {
        idle_quiet(&d, 3);          /* inter-tap gap, well inside the window */
        dial_result_t r = tap(&d);
        assert(r.kind == DIAL_NONE);
    }
    dial_result_t c = idle_quiet_until_commit(&d);
    assert(c.kind == DIAL_ABSOLUTE);
    assert(c.count == 8);
}

/* ----------------------------------------------------------------------------
 * 4. Nine-plus taps -> ABSOLUTE clamped at 8 (NEVER 9). This is the real gotcha:
 *    the FSM clamps the count so the caller's librarian_set_active(count-1) stays
 *    in 0..7 and never feeds 8 (-EINVAL) nor routes through lib_bank_clamp (which
 *    would reset >=8 to 0, a silent jump to profile 1).
 * --------------------------------------------------------------------------*/
static void test_nine_plus_taps_clamped_at_eight(void) {
    dial_t d;
    dial_init(&d);

    tap(&d);
    for (int i = 0; i < 11; i++) {  /* 12 taps total */
        idle_quiet(&d, 2);
        tap(&d);
    }
    dial_result_t c = idle_quiet_until_commit(&d);
    assert(c.kind == DIAL_ABSOLUTE);
    assert(c.count == 8);           /* pinned, never 9+ */
    assert(c.count - 1 <= 7);       /* the caller's index stays in 0..7 */
}

/* ----------------------------------------------------------------------------
 * 5. A tap, a long gap (> INTER_TAP_WINDOW), another tap -> TWO separate
 *    RELATIVE_NEXT results, NOT one ABSOLUTE(2). The burst ended after the gap.
 * --------------------------------------------------------------------------*/
static void test_tap_gap_tap_is_two_relatives(void) {
    dial_t d;
    dial_init(&d);

    dial_result_t r1 = tap(&d);
    assert(r1.kind == DIAL_NONE);   /* armed */

    /* gap longer than INTER_TAP_WINDOW: the lone tap resolves to RELATIVE_NEXT */
    dial_result_t first = idle_quiet_until_commit(&d);
    assert(first.kind == DIAL_RELATIVE_NEXT);

    /* rest of the idle is quiet (the burst is fully closed) */
    idle_quiet(&d, 10);

    /* a fresh tap starts a brand-new burst */
    dial_result_t r2 = tap(&d);
    assert(r2.kind == DIAL_NONE);
    dial_result_t second = idle_quiet_until_commit(&d);
    assert(second.kind == DIAL_RELATIVE_NEXT);
}

/* ----------------------------------------------------------------------------
 * 6. Commit fires only AFTER COMMIT_WINDOW idle ticks (not a tick early). Prove
 *    the absolute count is still pending right up to COMMIT_WINDOW-1, then fires.
 * --------------------------------------------------------------------------*/
static void test_commit_fires_only_after_commit_window(void) {
    dial_t d;
    dial_init(&d);

    tap(&d);
    idle_quiet(&d, 5);
    tap(&d);                        /* count = 2 */

    /* COMMIT_WINDOW - 1 idle ticks: still pending, nothing committed yet */
    idle_quiet(&d, COMMIT_WINDOW - 1);

    /* the COMMIT_WINDOW-th idle tick fires the commit */
    dial_result_t c = dial_step(&d, 0);
    assert(c.kind == DIAL_ABSOLUTE);
    assert(c.count == 2);
}

/* ----------------------------------------------------------------------------
 * 7. A SLOW but in-window count to 8 is never cut off early. A multi-tap burst
 *    commits COMMIT_WINDOW idle ticks after the LAST tap, so to chain taps the
 *    gap must stay UNDER COMMIT_WINDOW; each accepted tap resets the idle timer,
 *    and there is NO total cap, so an arbitrarily long count survives as long as
 *    no single gap reaches the commit threshold. We dial at a deliberately
 *    relaxed pace (COMMIT_WINDOW - 1 = the slowest gap that still chains) and
 *    prove all 8 taps land in one uncut ABSOLUTE(8). The guard band (COMMIT <
 *    INTER_TAP) guarantees this multi-tap commit can never collide with the
 *    lone-tap relative resolution (which waits the longer INTER_TAP_WINDOW). */
static void test_slow_in_window_count_to_eight_not_cut_off(void) {
    dial_t d;
    dial_init(&d);

    /* the slowest inter-tap gap that still keeps the burst alive: one tick under
     * the commit threshold. A gap of COMMIT_WINDOW would (correctly) commit. */
    int slow_gap = COMMIT_WINDOW - 1;
    assert(slow_gap > 0);

    tap(&d);
    for (int i = 0; i < 7; i++) {
        dial_result_t r = idle_quiet(&d, slow_gap);  /* gap < commit, no early fire */
        assert(r.kind == DIAL_NONE);
        r = tap(&d);                                 /* resets the idle timer */
        assert(r.kind == DIAL_NONE);
    }
    /* now let it idle out fully -> a single ABSOLUTE(8), uncut */
    dial_result_t c = idle_quiet_until_commit(&d);
    assert(c.kind == DIAL_ABSOLUTE);
    assert(c.count == 8);
}

/* ----------------------------------------------------------------------------
 * 8. Guard-band invariant the spec LOCKS: COMMIT_WINDOW must be < INTER_TAP_WINDOW
 *    so "burst still going" and "burst ended" never fight.
 * --------------------------------------------------------------------------*/
static void test_guard_band_invariant(void) {
    assert(COMMIT_WINDOW < INTER_TAP_WINDOW);
}

/* ----------------------------------------------------------------------------
 * PLAY LAYER COUNT-DIAL (#61 gesture pass, spec §2 / §4.2). A SECOND dial.c
 * instance drives PLAY layer select: 1 tap = RELATIVE +1, 2..8 taps = ABSOLUTE
 * jump to layer 2..8. The pure FSM is byte-for-byte the same as the •• profile
 * dial; the ONLY difference is at the COMMIT site in main.c, which clamps an
 * ABSOLUTE count to GESTURE_LAYER_COUNT (8): dial.c stays literal-8, so count N
 * SELECTS layer N for all 1..8 with no artificial pin, and >=9 taps clamp to 8 in
 * the FSM -> the top layer. These tests assert (a) the layer-dial counts main.c
 * will consume, and (b) the exact clamp expression main.c applies
 * (target = min(count-1, GESTURE_LAYER_COUNT-1)).
 * --------------------------------------------------------------------------*/

/* The clamp main.c applies to an ABSOLUTE layer-dial commit (spec §4.2).
 * Mirrors the main.c expression: target = count-1, pinned to GESTURE_LAYER_COUNT-1
 * (=7). Retied to GESTURE_LAYER_COUNT so it cannot drift from gesture.h. Kept here
 * so the host suite covers the clamp even though its live site is a few lines of
 * main.c routing (not a pure FSM). */
#define LAYER_MAX_INDEX (GESTURE_LAYER_COUNT - 1)   /* 8 layers are 0..7 */
static int layer_clamp(int count) {
    int target = count - 1;
    if (target > LAYER_MAX_INDEX) target = LAYER_MAX_INDEX;
    return target;
}

/* 1 PLAY tap -> RELATIVE_NEXT (caller does layer = (layer+1) % GESTURE_LAYER_COUNT).
 * Identical to the •• lone-tap path; the layer-dial reuses the exact same FSM
 * instance. */
static void test_layer_one_tap_is_relative_next(void) {
    dial_t d;
    dial_init(&d);
    dial_result_t r = tap(&d);
    assert(r.kind == DIAL_NONE);          /* armed, could still upgrade */
    idle_quiet(&d, INTER_TAP_WINDOW);     /* ride out the inter-tap window */
    dial_result_t rel = dial_step(&d, 0); /* the tick it expires -> relative */
    assert(rel.kind == DIAL_RELATIVE_NEXT);
}

/* 2/3/4 PLAY taps -> ABSOLUTE with count 2/3/4. The caller jumps to layer
 * count-1 (1-indexed like the profile dial), clamped to GESTURE_LAYER_COUNT-1.
 * Verify each count and its post-clamp target index (2->1, 3->2, 4->3, all in
 * range, no clamping yet). */
static void test_layer_two_three_four_taps_absolute(void) {
    for (int n = 2; n <= 4; n++) {
        dial_t d;
        dial_init(&d);
        tap(&d);
        for (int i = 1; i < n; i++) {
            idle_quiet(&d, 3);            /* inter-tap gap, well inside the window */
            dial_result_t r = tap(&d);
            assert(r.kind == DIAL_NONE);
        }
        dial_result_t c = idle_quiet_until_commit(&d);
        assert(c.kind == DIAL_ABSOLUTE);
        assert(c.count == n);             /* 2/3/4 -> absolute count 2/3/4 */
        /* the layer index main.c lands on: count-1, no clamp needed for 2..4 */
        assert(layer_clamp(c.count) == n - 1);
        assert(layer_clamp(c.count) >= 0 && layer_clamp(c.count) <= LAYER_MAX_INDEX);
    }
}

/* 5..8 PLAY taps -> SELECT layers 5..8 (index count-1). With GESTURE_LAYER_COUNT=8
 * the main.c absolute clamp ceiling is 7 = DIAL_MAX_COUNT-1, so count N maps to
 * layer N for all 1..8 with no artificial pin (spec §4.2). count N -> track N. */
static void test_layer_five_to_eight_taps_select_their_layer(void) {
    for (int n = 5; n <= 8; n++) {
        dial_t d;
        dial_init(&d);
        tap(&d);
        for (int i = 1; i < n; i++) {
            idle_quiet(&d, 3);
            tap(&d);
        }
        dial_result_t c = idle_quiet_until_commit(&d);
        assert(c.kind == DIAL_ABSOLUTE);
        assert(c.count == n);             /* FSM (literal-8) emits the real count */
        assert(layer_clamp(c.count) == n - 1);             /* SELECT layer 5..8 */
        assert(layer_clamp(c.count) <= LAYER_MAX_INDEX);
    }
}

/* Mirrors main.c:1004 relative +1 wrap: layer = (layer+1) % GESTURE_LAYER_COUNT.
 * Locks the spec §4.2 decision to KEEP the modulo wrap at 8 (single-tap +1 from
 * layer 8 wraps to layer 1); stop-at-8 is NOT adopted. */
static int layer_next(int cur) {
    return (cur + 1) % GESTURE_LAYER_COUNT;
}

/* 1 PLAY tap = relative +1. Mid-range steps up with no wrap; only the TOP layer
 * (index 7) wraps back to layer 1 (index 0). At 4 layers index 3 was the top and
 * wrapped to 0 - proving layer_next(3)==4 is the 8-layer behavior. */
static void test_layer_single_tap_wrap(void) {
    assert(layer_next(3) == 4);
    assert(layer_next(6) == 7);
    assert(layer_next(7) == 0);   /* wrap 8 -> 1 (spec §4.2) */
    assert(layer_next(0) == 1);
}

/* >=9 PLAY taps: the FSM clamps count to 8 (its hard cap), and main.c then clamps
 * the layer to the top (index 7), a double clamp that can never index past the
 * side LED row. */
static void test_layer_nine_plus_taps_pin_to_top_layer(void) {
    dial_t d;
    dial_init(&d);
    tap(&d);
    for (int i = 0; i < 11; i++) {        /* 12 taps total */
        idle_quiet(&d, 2);
        tap(&d);
    }
    dial_result_t c = idle_quiet_until_commit(&d);
    assert(c.kind == DIAL_ABSOLUTE);
    assert(c.count == DIAL_MAX_COUNT);    /* FSM pins at 8 */
    assert(layer_clamp(c.count) == LAYER_MAX_INDEX);   /* main.c pins to top layer */
}

/* dial_profile_peek_pattern: active index N renders profile (N+1) via
 * dial_track_pattern - 3 solid for index 2, and index 5 (count 6) blinks LED idx1. */
static void test_profile_peek_pattern(void){
    unsigned char out[4];
    dial_profile_peek_pattern(2, 0, out);              /* profile 3 */
    assert(out[0]==1 && out[1]==1 && out[2]==1 && out[3]==0);
    dial_profile_peek_pattern(5, 0, out);              /* profile 6, tick 0 (blink off) */
    assert(out[0]==1 && out[1]==0 && out[2]==1 && out[3]==1);
    dial_profile_peek_pattern(5, 12, out);             /* tick 12 (blink on) */
    assert(out[0]==1 && out[1]==1 && out[2]==1 && out[3]==1);
}

int main(void) {
    test_lone_tap_is_relative_next();
    test_profile_peek_pattern();
    test_two_fast_taps_absolute_two();
    test_eight_fast_taps_absolute_eight();
    test_nine_plus_taps_clamped_at_eight();
    test_tap_gap_tap_is_two_relatives();
    test_commit_fires_only_after_commit_window();
    test_slow_in_window_count_to_eight_not_cut_off();
    test_guard_band_invariant();
    /* PLAY layer count-dial (#61): reuse of the same FSM + the main.c clamp-to-4 */
    test_layer_one_tap_is_relative_next();
    test_layer_two_three_four_taps_absolute();
    test_layer_five_to_eight_taps_select_their_layer();
    test_layer_single_tap_wrap();
    test_layer_nine_plus_taps_pin_to_top_layer();
    printf("all dial tests passed\n");
    return 0;
}
