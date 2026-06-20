#ifndef GESTURE_H
#define GESTURE_H
#include <stdint.h>

/* Pure, host-testable PLAY gesture FSM for the SP-1 (design spec §3,
 * docs/superpowers/specs/2026-06-19-feldd-indicator-language-design.md).
 *
 * Stepped exactly once per main-loop scan tick (~8 ms). Time is expressed in
 * scan ticks (NOT wall-clock) so the same translation unit links into both the
 * firmware and the host test, exactly like control_logic.c / gesture.c's old
 * pure core.
 *
 * It used to be press-only/release-blind (a double-tap-PLAY shift latch). It now
 * sees the press edge, the debounced hold, and a "rocker arrived" signal, so it
 * can tell three PLAY gestures apart on ONE button:
 *
 *   double-tap PLAY (inside the window)   -> GESTURE_LAYER_STEP  (step the layer)
 *   hold PLAY alone past the peek plateau -> GESTURE_PEEK_PROFILE (fires on RELEASE)
 *   hold PLAY + a FWD/RWD rocker          -> mode switch (NOT a gesture.c output;
 *                                            main.c/mode_decide owns it; the rocker
 *                                            only VOIDS the would-be peek here)
 *
 * PLAY lives on button index 0; this gesture intentionally lives on PLAY (not the
 * overloaded •• button) so it never collides with the •• tap(profile-dial)/
 * hold(power-off) gestures. */

/* Two PLAY presses within this many scan ticks count as a double-tap. The loop
 * runs ~8 ms/tick, so 45 ticks ~= 360 ms. Must stay comfortably above the
 * ~24 ms (3-tick) ladder debounce floor (buttons.c) so a real tap-release-tap
 * can be resolved. Bench-tunable once the Play plateau is calibrated. */
#define DOUBLE_TAP_WINDOW_SCANS 45

/* A PLAY hold must persist this long (≥ ~1.6× the double-tap window) to ARM a
 * profile peek. 75 ticks ~= 600 ms. Chosen by the adversarial lens (50->75) so a
 * slow double-tap's inter-press idle is never misread and the peek is a
 * deliberate hold, not an accidental long press. Far under the 375-tick (~3 s) ••
 * power-off and the ~150-tick DFU hold (and those are different buttons anyway:
 * ••=GPIO, DFU=Track1+4). Bench-tunable. */
#define PEEK_HOLD_SCANS 75

/* Number of layers the double-tap cycles through (`layer = (layer+1) % N`). 2
 * today (LED1/LED2); the deferred 4-layer feature bumps this to 4 (LED3/LED4).
 * gesture.c composes with, but does not require, the 4-layer work. */
#define GESTURE_LAYER_COUNT 2

/* gesture_step() return codes. */
#define GESTURE_NONE          0
#define GESTURE_LAYER_STEP    1  /* double-tap PLAY stepped the layer (was LATCH_TOGGLED) */
#define GESTURE_PEEK_PROFILE  2  /* hold-PLAY-alone released past the peek plateau */

typedef struct {
    uint8_t  layer;        /* engaged layer index 0..GESTURE_LAYER_COUNT-1 */
    uint8_t  armed;        /* a first PLAY press is waiting for a possible second */
    uint8_t  play_was_down;/* hold state on the PREVIOUS tick (for rise/fall edges) */
    uint8_t  rocker_seen;  /* a FWD/RWD was consumed during THIS hold (voids the peek) */
    uint8_t  peek_pending; /* a peek has ARMED this hold; commits on release */
    uint16_t since_press;  /* scan ticks since the last PLAY press (saturating) */
    uint16_t hold_ticks;   /* scan ticks PLAY has been continuously held (saturating, 0 on release) */
} gesture_t;

void gesture_init(gesture_t *g);

/* Advance one scan tick (design spec §3, per-tick steps 1..5).
 *   play_press                - 1 on the tick a PLAY press EDGE was seen, else 0.
 *   play_held                 - the DEBOUNCED hold: 1 while PLAY is held down
 *                               (main.c feeds buttons_track_committed()==0).
 *   rocker_consumed_this_hold - 1 on the tick mode_decide consumed a FWD/RWD
 *                               during a PLAY hold (the "a rocker arrived" signal;
 *                               this folds in the old gesture_disarm trigger).
 * Returns GESTURE_LAYER_STEP on a completed double-tap, GESTURE_PEEK_PROFILE on
 * the RELEASE tick of a hold-alone peek, else GESTURE_NONE. */
int gesture_step(gesture_t *g, int play_press, int play_held,
                 int rocker_consumed_this_hold);

/* Current engaged layer (0..GESTURE_LAYER_COUNT-1). */
int gesture_layer(const gesture_t *g);

/* Back-compat shift accessor: the layer's low bit (0/1). Feed as the `shift` arg
 * into map_fader/map_button for a 2-layer build (layer 1 = shift bank). Kept so
 * main.c's existing shift-bank read keeps working through the layer rename.
 * gesture_latched() is an alias for the same value. */
int gesture_shift(const gesture_t *g);
int gesture_latched(const gesture_t *g);

/* Cancel a pending first-tap arm WITHOUT touching the engaged layer. Thin wrapper
 * over the inline disarm now folded into gesture_step under
 * rocker_consumed_this_hold; kept so existing call sites + host tests still pass.
 * Clears armed + since_press (and any pending peek), preserves the layer. */
void gesture_disarm(gesture_t *g);

#endif /* GESTURE_H */
