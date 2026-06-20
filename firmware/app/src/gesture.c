#include "gesture.h"

void gesture_init(gesture_t *g) {
    g->layer = 0;
    g->armed = 0;
    g->play_was_down = 0;
    g->rocker_seen = 0;
    g->peek_pending = 0;
    g->since_press = 0;
    g->hold_ticks = 0;
}

int gesture_step(gesture_t *g, int play_press, int play_held,
                 int rocker_consumed_this_hold) {
    /* §3 step 1: advance the inter-tap timer (saturate so it can't wrap) and
     * expire a stale double-tap arm once the window passes with no second tap. */
    if (g->since_press < 0xFFFF) {
        g->since_press++;
    }
    if (g->armed && g->since_press > DOUBLE_TAP_WINDOW_SCANS) {
        g->armed = 0;
    }

    /* §3 step 2: track the hold. rise/fall edges off play_was_down. */
    int held = play_held ? 1 : 0;
    int rose = held && !g->play_was_down;
    int fell = !held && g->play_was_down;

    if (rose) {
        /* fresh hold: clear the per-hold latches */
        g->rocker_seen = 0;
        g->peek_pending = 0;
    }
    if (held) {
        if (g->hold_ticks < 0xFFFF) {
            g->hold_ticks++;
        }
    } else {
        g->hold_ticks = 0;
    }

    /* a FWD/RWD consumed mid-hold is a MODE switch, not a peek: latch rocker_seen,
     * clear the double-tap arm, and void any pending peek (folds in gesture_disarm). */
    if (rocker_consumed_this_hold) {
        g->rocker_seen = 1;
        g->armed = 0;
        g->peek_pending = 0;
    }

    g->play_was_down = held;

    /* §3 step 3: double-tap -> LAYER_STEP on the 2nd press inside the window. */
    if (play_press) {
        if (g->armed && g->since_press <= DOUBLE_TAP_WINDOW_SCANS) {
            /* second tap inside the window -> step the layer */
            g->layer = (uint8_t)((g->layer + 1) % GESTURE_LAYER_COUNT);
            g->armed = 0;
            g->hold_ticks = 0;     /* a tap, not a hold: don't let it arm a peek */
            g->since_press = 0;
            return GESTURE_LAYER_STEP;
        }
        /* first tap (or too-late tap) -> arm and wait for a possible second */
        g->armed = 1;
        g->since_press = 0;
    }

    /* §3 step 4: ARM a peek (arm only, do NOT fire yet) once a clean hold (no
     * rocker, no pending double-tap arm) has persisted exactly PEEK_HOLD_SCANS. */
    if (play_held && !g->rocker_seen && !g->armed &&
        g->hold_ticks == PEEK_HOLD_SCANS) {
        g->peek_pending = 1;
    }

    /* §3 step 5: COMMIT the peek on RELEASE if it armed and no rocker voided it. */
    if (fell && g->peek_pending && !g->rocker_seen) {
        g->peek_pending = 0;
        return GESTURE_PEEK_PROFILE;
    }

    return GESTURE_NONE;
}

int gesture_layer(const gesture_t *g) {
    return g->layer;
}

int gesture_shift(const gesture_t *g) {
    return g->layer & 1;
}

int gesture_latched(const gesture_t *g) {
    return gesture_shift(g);
}

void gesture_disarm(gesture_t *g) {
    /* Cancel any pending first-tap arm + pending peek WITHOUT disturbing the
     * engaged layer. The rocker-consumed path inside gesture_step already does
     * this inline; this thin wrapper keeps existing call sites + the host test
     * working. Clear armed + since_press + peek_pending, preserve layer. */
    g->armed = 0;
    g->since_press = 0;
    g->peek_pending = 0;
}
