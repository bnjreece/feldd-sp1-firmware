#include "dial.h"

/* min() so the count clamps to DIAL_MAX_COUNT (8) INSIDE the FSM. This is the
 * spec §8 gotcha: the caller does librarian_set_active(count-1) in 0..7, so the
 * FSM must never produce a count whose count-1 exceeds 7. We clamp here and NEVER
 * route the raw count through lib_bank_clamp() — that resets >=8 to 0 (not 8),
 * which would silently jump a stray 8-tap to profile 1. Over-tap past 8 pins at
 * count 8 / index 7 (clamp, no wrap). */
static uint8_t dmin(uint8_t a, uint8_t b) { return a < b ? a : b; }

/* Compile-time guard band: COMMIT_WINDOW MUST be < INTER_TAP_WINDOW so a gap that
 * keeps a burst alive (< INTER_TAP) can never also satisfy "burst ended" (idle
 * >= COMMIT). If these ever cross, "still going" and "ended" fight. (C99 negative
 * array-size trick; costs nothing at runtime.) */
typedef char dial_guard_band_assert[(COMMIT_WINDOW < INTER_TAP_WINDOW) ? 1 : -1];

void dial_init(dial_t *d) {
    d->active = 0;
    d->count = 0;
    d->since_tap = 0;
}

dial_result_t dial_step(dial_t *d, int tap_edge) {
    dial_result_t r = { DIAL_NONE, 0 };

    /* Advance the inter-tap idle timer first (saturate so it can't wrap). */
    if (d->since_tap < 0xFFFF) {
        d->since_tap++;
    }

    if (tap_edge) {
        /* A valid short •• tap release landed this tick. Accept it into the
         * current burst (or start a fresh one) and reset the idle timer.
         *
         * TIMING CHOICE (instant-vs-upgrade, spec §8): we do NOT emit the lone
         * RELATIVE_NEXT on this very tick. If we fired immediately, a 2nd tap
         * arriving inside INTER_TAP_WINDOW could not upgrade the burst to an
         * absolute jump without first having (wrongly) cycled +1. So a 1st tap
         * only ARMS the burst; the relative result is held until the inter-tap
         * window expires with exactly one tap (see the expiry block below).
         * Correctness of the count beats the ~300 ms of relative latency, and
         * that latency is identical to the absolute path's commit feel, so the
         * button behaves consistently. (If a future bench pass wants the lone
         * tap to feel instant, it must commit on the edge AND retroactively
         * correct on a 2nd tap — strictly more state; deferred.) */
        d->active = 1;
        if (d->count < DIAL_MAX_COUNT) {
            d->count++;
        }
        d->count = dmin(d->count, DIAL_MAX_COUNT);  /* belt-and-suspenders clamp */
        d->since_tap = 0;
        return r;  /* always quiet on a tap tick; resolution happens on idle */
    }

    /* No tap this tick. If a burst is in flight, see whether it has resolved. */
    if (d->active) {
        if (d->count == 1) {
            /* Lone-tap burst. It resolves to RELATIVE_NEXT once the inter-tap
             * window passes with no 2nd tap (no separate commit window for the
             * single tap — the inter-tap expiry IS its commit). */
            if (d->since_tap > INTER_TAP_WINDOW) {
                r.kind = DIAL_RELATIVE_NEXT;
                r.count = 0;
                d->active = 0;
                d->count = 0;
            }
        } else {
            /* Multi-tap burst (count is 2..8). It commits ABSOLUTE after
             * COMMIT_WINDOW idle ticks with no further tap. Because COMMIT_WINDOW
             * < INTER_TAP_WINDOW (guard band), a gap that is still "within the
             * burst" (< INTER_TAP) never reaches the commit threshold mid-count,
             * so a slow-but-in-window count to 8 is never cut off early. */
            if (d->since_tap >= COMMIT_WINDOW) {
                r.kind = DIAL_ABSOLUTE;
                r.count = dmin(d->count, DIAL_MAX_COUNT);  /* 1..8, never 9+ */
                d->active = 0;
                d->count = 0;
            }
        }
    }

    return r;
}

/* spec §8 LED feedback (TRACK ROW ONLY). Pure count -> 4-LED pattern. See the
 * dial.h doc comment for the full contract. */
void dial_track_pattern(unsigned char count, unsigned char tick,
                        unsigned char out[4]) {
    int i;

    /* count 0 = idle = all off (caller falls back to layer-rest). */
    if (count == 0) {
        for (i = 0; i < 4; i++) {
            out[i] = 0;
        }
        return;
    }

    /* Clamp to DIAL_MAX_COUNT (8): the FSM already pins here, but guard the LED
     * path independently so a stray >8 can never index past the track row. */
    if (count > DIAL_MAX_COUNT) {
        count = DIAL_MAX_COUNT;
    }

    if (count <= 4) {
        /* counts 1-4: that many LEDs solid, building left->right. */
        for (i = 0; i < 4; i++) {
            out[i] = (i < count) ? 1 : 0;
        }
    } else {
        /* counts 5-8: all four solid, but blink the (count-4)th LED, i.e.
         * index count-5. Same ~12-tick half-period as main.c's charge blink. */
        int blink_idx = count - 5;            /* 5->0, 6->1, 7->2, 8->3 */
        int lit = (tick / 12) & 1;            /* current blink phase: 1 = lit */
        for (i = 0; i < 4; i++) {
            out[i] = (i == blink_idx) ? (unsigned char)lit : 1;
        }
    }
}

/* Feature 4: the func-mode profile peek reuses dial_track_pattern to render the
 * 1-based profile number (active_index + 1) on the track row. */
void dial_profile_peek_pattern(unsigned char active_index, unsigned char tick,
                               unsigned char out[4]) {
    dial_track_pattern((unsigned char)(active_index + 1), tick, out);
}
