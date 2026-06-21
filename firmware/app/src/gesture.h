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
 * can tell the PLAY gestures apart on ONE button:
 *
 *   hold PLAY alone past the peek plateau -> GESTURE_PEEK_PROFILE (fires on RELEASE)
 *   hold PLAY + a FWD/RWD rocker          -> mode switch (NOT a gesture.c output;
 *                                            main.c/mode_decide owns it; the rocker
 *                                            only VOIDS the would-be peek here)
 *
 * LAYER select moved OFF this FSM (#61 gesture pass, spec
 * notes/2026-06-20-feldd-gesture-pass-spec.md §2): the old double-tap ->
 * GESTURE_LAYER_STEP round-robin is REPLACED by a second dial.c count-dial
 * instance in main.c (1 tap = +1, 2/3/4 taps = absolute jump to layer 2/3/4).
 * The engaged layer still LIVES here (read via gesture_layer(), the side row's
 * source of truth), but it is now SET by main.c via gesture_set_layer() on a
 * dial commit instead of stepped by a double-tap. gesture.c keeps ONLY the peek
 * FSM + the rocker-voids-peek latch.
 *
 * PLAY lives on button index 0; this gesture intentionally lives on PLAY (not the
 * overloaded •• button) so it never collides with the •• tap(profile-dial)/
 * hold(power-off) gestures. */

/* A PLAY hold must persist this long to ARM a profile peek. 75 ticks ~= 600 ms.
 * Chosen by the adversarial lens (50->75) so a slow tap-burst's inter-press idle
 * is never misread and the peek is a deliberate hold, not an accidental long
 * press. The peek band (arms at 75) is disjoint from the PLAY layer-dial tap band
 * (a tap is a press RELEASED in 1..40 ticks, classified in main.c), so a dial tap
 * never arms a peek and a peek hold is never a dial tap. Far under the 625-tick
 * (~5 s) •• power-off and the ~150-tick DFU hold (different buttons anyway:
 * ••=GPIO, DFU=Track1+4). Bench-tunable. */
#define PEEK_HOLD_SCANS 75

/* Number of layers (`layer` is 0..GESTURE_LAYER_COUNT-1). v5 raised this 2 -> 4
 * so the side LEDs map 1->2->3->4 for layers 0->1->2->3. The layer is now SET by
 * main.c's layer count-dial (gesture_set_layer); the relative +1 wrap and the
 * 2/3/4-tap absolute clamp both honor this ceiling. */
#define GESTURE_LAYER_COUNT 4

/* gesture_step() return codes. */
#define GESTURE_NONE          0
#define GESTURE_PEEK_PROFILE  2  /* hold-PLAY-alone released past the peek plateau */

typedef struct {
    uint8_t  layer;        /* engaged layer index 0..GESTURE_LAYER_COUNT-1 (set by gesture_set_layer) */
    uint8_t  play_was_down;/* hold state on the PREVIOUS tick (for rise/fall edges) */
    uint8_t  rocker_seen;  /* a FWD/RWD was consumed during THIS hold (voids the peek) */
    uint8_t  peek_pending; /* a peek has ARMED this hold; commits on release */
    uint16_t hold_ticks;   /* scan ticks PLAY has been continuously held (saturating, 0 on release) */
} gesture_t;

void gesture_init(gesture_t *g);

/* Advance one scan tick (peek FSM only now; the layer double-tap is gone).
 *   play_held                 - the DEBOUNCED hold: 1 while PLAY is held down
 *                               (main.c feeds buttons_track_committed()==0).
 *   rocker_consumed_this_hold - 1 on the tick mode_decide consumed a FWD/RWD
 *                               during a PLAY hold (the "a rocker arrived" signal;
 *                               this folds in the old gesture_disarm trigger).
 * Returns GESTURE_PEEK_PROFILE on the RELEASE tick of a hold-alone peek, else
 * GESTURE_NONE. (Layer select is now main.c's layer count-dial, committed via
 * gesture_set_layer; this FSM no longer touches the layer or the press edge.) */
int gesture_step(gesture_t *g, int play_held,
                 int rocker_consumed_this_hold);

/* Current engaged layer (0..GESTURE_LAYER_COUNT-1). */
int gesture_layer(const gesture_t *g);

/* Set the engaged layer (0..GESTURE_LAYER_COUNT-1). Called by main.c's layer
 * count-dial on a RELATIVE_NEXT (+1, caller wraps) or ABSOLUTE (clamped) commit;
 * out-of-range values are clamped to the top layer so a stray over-tap can never
 * index past the side LED row. Side-row layer LED snaps to the new value on the
 * next render tick (commit-only, no live mid-burst count — spec §2). */
void gesture_set_layer(gesture_t *g, int layer);

/* Back-compat shift accessor: the layer's low bit (0/1). Feed as the `shift` arg
 * into map_fader/map_button for a 2-layer build (layer 1 = shift bank). Kept so
 * main.c's existing shift-bank read keeps working through the layer rename.
 * gesture_latched() is an alias for the same value. */
int gesture_shift(const gesture_t *g);
int gesture_latched(const gesture_t *g);

/* Cancel a pending peek WITHOUT touching the engaged layer. Thin wrapper over the
 * inline disarm now folded into gesture_step under rocker_consumed_this_hold;
 * kept so existing call sites + host tests still pass. Clears peek_pending only
 * (the old first-tap arm is gone), preserves the layer. */
void gesture_disarm(gesture_t *g);

#endif /* GESTURE_H */
