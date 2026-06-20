#ifndef DIAL_H
#define DIAL_H
#include <stdint.h>

/* Pure, host-testable HYBRID count-dial for the SP-1 •• profile button
 * (design spec §8, LOCKED 2026-06-19). Replaces the old blocking single-tap
 * round-robin with a count-based, bidirectional, eyes-off dial:
 *
 *   1 tap          -> RELATIVE +1   (today's within-bank round-robin)
 *   2..8 quick taps-> ABSOLUTE jump to within-bank profile = count (1..8)
 *
 * Stepped exactly once per main-loop scan tick (~8 ms). Time is in SCAN TICKS,
 * never wall-clock, so the same translation unit links into both the firmware
 * and the host test, exactly like gesture.c / control_logic.c.
 *
 * main.c owns the tap CLASSIFIER (held 1..MAX_TAP_TICKS = a tap; dead band
 * 62..374; power-off >=375) and feeds this FSM `tap_edge = 1` on the single tick
 * a valid short dot-dot tap RELEASE was classified. This FSM owns only the
 * burst timing: counting taps, deciding relative-vs-absolute, and clamping. */

/* --- Thresholds (tick ~8 ms; bench-tunable per gesture.h's existing note) --- */

/* Max gap between taps that keeps a burst alive (soft idle timer, reset by each
 * accepted tap). NO total cap -> a slow count-to-8 is never cut off. Reuses the
 * gesture.c double-tap window (45 ticks ~= 360 ms). */
#define INTER_TAP_WINDOW 45

/* Idle ticks after the LAST tap before an ABSOLUTE count commits (~300 ms).
 * MUST be < INTER_TAP_WINDOW so "burst still going" (gap < INTER_TAP) and
 * "burst ended" (idle >= COMMIT) never fight — an 8-tick guard band. This is the
 * exact resolution of "quick, but do not cut off a slow count" (spec §8).
 * The lone (relative) tap pays NO commit window: it commits on the tap edge once
 * the inter-tap window expires with exactly one tap. */
#define COMMIT_WINDOW 37

/* Hard ceiling on the count: NUM_BANK_PROFILES (8 within-bank profiles). Kept as
 * a literal here so dial.c/dial.h stay freestanding (no profile.h needed for the
 * pure core / host test); profile.h's NUM_BANK_PROFILES is also 8 and the build
 * asserts them equal where it includes both. */
#define DIAL_MAX_COUNT 8

/* dial_step() result kinds. */
#define DIAL_NONE          0  /* nothing resolved this tick */
#define DIAL_RELATIVE_NEXT 1  /* lone tap: caller does lib_bank_cycle (+1)      */
#define DIAL_ABSOLUTE      2  /* burst: caller does librarian_set_active(count-1)*/

typedef struct {
    uint8_t  kind;   /* DIAL_NONE | DIAL_RELATIVE_NEXT | DIAL_ABSOLUTE */
    uint8_t  count;  /* for DIAL_ABSOLUTE: the profile number 1..8 (index = count-1) */
} dial_result_t;

typedef struct {
    uint8_t  active;     /* a burst is in progress (>=1 tap seen, not yet resolved) */
    uint8_t  count;      /* taps accepted in the current burst (clamped to DIAL_MAX_COUNT) */
    uint16_t since_tap;  /* scan ticks since the last accepted tap (saturating) */
} dial_t;

void dial_init(dial_t *d);

/* Advance one scan tick. tap_edge is 1 on the tick main.c classified a valid
 * short •• tap RELEASE, else 0. Returns:
 *   DIAL_RELATIVE_NEXT — the tick a lone-tap burst resolves (inter-tap window
 *                        expired with exactly one tap). Caller cycles +1.
 *   DIAL_ABSOLUTE      — the tick a 2..8-tap burst commits (COMMIT_WINDOW idle
 *                        after the last tap). result.count is 1..8 (caller does
 *                        librarian_set_active(count-1), in 0..7).
 *   DIAL_NONE          — otherwise (burst still arming/counting, or fully idle). */
dial_result_t dial_step(dial_t *d, int tap_edge);

/* Map a dial COUNT to the TRACK-ROW LED pattern (design spec §8, "LED feedback —
 * TRACK ROW ONLY"). Pure: writes out[0..3] = 0/1 for track LED i. `tick` is the
 * current scan tick, used only for the 5-8 blink phase. Never touches the PLAY
 * row (that stays glanceable for MODE the whole dial).
 *
 *   count 0      -> all off (idle; the caller falls back to layer-rest).
 *   count 1..4   -> that many LEDs SOLID, building left->right (1 = index 0;
 *                   4 = all four). Tick-independent.
 *   count 5..8   -> all four SOLID, but the (count-4)th LED (index count-5)
 *                   BLINKS: lit on the tick phase (tick/12)&1, dark otherwise.
 *                   So 5 blinks index 0 ... 8 blinks index 3.
 *
 * The ~12-tick half-period matches main.c's charge/profile blink (main.c:319,
 * ((++blink / 12u) & 1u)). Counts >8 clamp to 8 (the FSM already pins there);
 * any unexpected count >8 is treated as 8 for safety. */
void dial_track_pattern(unsigned char count, unsigned char tick,
                        unsigned char out[4]);

#endif /* DIAL_H */
