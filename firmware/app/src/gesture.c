#include "gesture.h"

void gesture_init(gesture_t *g) {
    g->layer = 0;
    g->play_was_down = 0;
    g->rocker_seen = 0;
    g->peek_pending = 0;
    g->hold_ticks = 0;
}

int gesture_step(gesture_t *g, int play_held,
                 int rocker_consumed_this_hold) {
    /* step 1: track the hold. rise/fall edges off play_was_down. */
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

    /* a FWD/RWD consumed mid-hold is a MODE switch, not a peek: latch rocker_seen
     * and void any pending peek (folds in gesture_disarm). */
    if (rocker_consumed_this_hold) {
        g->rocker_seen = 1;
        g->peek_pending = 0;
    }

    g->play_was_down = held;

    /* step 2: ARM a peek (arm only, do NOT fire yet) once a clean hold (no rocker)
     * has persisted exactly PEEK_HOLD_SCANS. The PLAY layer-dial taps (release in
     * 1..40 ticks, classified in main.c) never reach this band (arms at 75). */
    if (play_held && !g->rocker_seen &&
        g->hold_ticks == PEEK_HOLD_SCANS) {
        g->peek_pending = 1;
    }

    /* step 3: COMMIT the peek on RELEASE if it armed and no rocker voided it. */
    if (fell && g->peek_pending && !g->rocker_seen) {
        g->peek_pending = 0;
        return GESTURE_PEEK_PROFILE;
    }

    return GESTURE_NONE;
}

int gesture_layer(const gesture_t *g) {
    return g->layer;
}

void gesture_set_layer(gesture_t *g, int layer) {
    /* Clamp into 0..GESTURE_LAYER_COUNT-1. main.c's layer count-dial already
     * clamps an over-tap to 4 (index 3) before calling, but guard here too so a
     * stray value can never index past the side LED row. */
    if (layer < 0) {
        layer = 0;
    } else if (layer > GESTURE_LAYER_COUNT - 1) {
        layer = GESTURE_LAYER_COUNT - 1;
    }
    g->layer = (uint8_t)layer;
}

int gesture_shift(const gesture_t *g) {
    return g->layer & 1;
}

int gesture_latched(const gesture_t *g) {
    return gesture_shift(g);
}

void gesture_disarm(gesture_t *g) {
    /* Cancel any pending peek WITHOUT disturbing the engaged layer. The
     * rocker-consumed path inside gesture_step already does this inline; this
     * thin wrapper keeps existing call sites + the host test working. (The old
     * double-tap first-tap arm is gone, so this only clears peek_pending now;
     * the engaged layer, owned by gesture_set_layer, is preserved.) */
    g->peek_pending = 0;
}
