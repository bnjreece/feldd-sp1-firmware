#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "mode.h"

/* Feature 4 retired mode_decide + the mode.c gesture template: the dot-dot+T4 mode
 * flip is now a genuine combo toggle (combo_dispatch + mode_toggle), so Keyboard
 * stays reachable and the flip cannot double-fire. These tests pin the combo engine
 * (press-side dispatch, single-fire toggle, per-index consume-both-edges latch) and
 * the surviving mode_led_pattern table. */

/* The N-mode LED pattern table (renders on the PLAY/side row, SP1_PLAY_LED1..4).
 * MIDI lights 1+4, KEYBOARD lights 2+3. Retained for side_led.c. */
static void t_led_pattern_table(void)
{
    assert(mode_led_pattern(MODE_MIDI)     == (MODE_LED1 | MODE_LED4));
    assert(mode_led_pattern(MODE_KEYBOARD) == (MODE_LED2 | MODE_LED3));
    /* reserved modes return a non-crashing pattern (modes 2,3 reserved). */
    assert(mode_led_pattern(2) == (MODE_LED1 | MODE_LED2));
    assert(mode_led_pattern(3) == (MODE_LED3 | MODE_LED4));
    /* out-of-range clamps to MIDI's pattern (defensive). */
    assert(mode_led_pattern(99) == (MODE_LED1 | MODE_LED4));
}

/* combo_dispatch: while func_down, a PRESS on idx 4..8 is consumed as a combo,
 * carries the right action, and signals reset_hold (clean-hold power-off gate).
 * A PRESS with func_down NOT held, or on a non-combo idx, is passthrough. */
static void t_combo_press_maps_and_consumes(void){
    combo_latch_t l; combo_latch_init(&l);
    struct combo_decision d;
    d = combo_dispatch(&l, 1, COMBO_BTN_T4, 1);
    assert(d.consumed==1 && d.action==COMBO_MODE_TOGGLE && d.reset_hold==1);
    d = combo_dispatch(&l, 1, COMBO_BTN_VOLUP, 1); assert(d.consumed==1 && d.action==COMBO_PROFILE_NEXT);
    d = combo_dispatch(&l, 1, COMBO_BTN_VOLDN, 1); assert(d.consumed==1 && d.action==COMBO_PROFILE_PREV);
    d = combo_dispatch(&l, 1, COMBO_BTN_FWD, 1);   assert(d.consumed==1 && d.action==COMBO_LAYER_NEXT);
    d = combo_dispatch(&l, 1, COMBO_BTN_RWD, 1);   assert(d.consumed==1 && d.action==COMBO_LAYER_PREV);
    /* func not held: plain button, no consume, no reset_hold */
    combo_latch_init(&l);
    d = combo_dispatch(&l, 0, COMBO_BTN_FWD, 1); assert(d.consumed==0 && d.action==COMBO_NONE && d.reset_hold==0);
    /* func held but a non-combo idx (PLAY / Track1..3): passthrough */
    d = combo_dispatch(&l, 1, 0, 1); assert(d.consumed==0);
    d = combo_dispatch(&l, 1, 1, 1); assert(d.consumed==0);
}
/* mode_toggle flips MIDI<->KEYBOARD; combo T4 fires the toggle exactly once
 * (press only), so no double-toggle. */
static void t_mode_toggle_and_single_fire(void){
    assert(mode_toggle(MODE_MIDI)==MODE_KEYBOARD);
    assert(mode_toggle(MODE_KEYBOARD)==MODE_MIDI);
    combo_latch_t l; combo_latch_init(&l);
    struct combo_decision p = combo_dispatch(&l, 1, COMBO_BTN_T4, 1);   /* press */
    struct combo_decision r = combo_dispatch(&l, 1, COMBO_BTN_T4, 0);   /* release */
    assert(p.action==COMBO_MODE_TOGGLE);           /* toggle only on the press */
    assert(r.consumed==1 && r.action==COMBO_NONE); /* release swallowed, no 2nd toggle */
}

/* Forward ordering: •• released BEFORE the combo button. The PRESS was consumed
 * (func_down), so the RELEASE must still be swallowed even though func_down is
 * now 0 - no stray release leaks to the button's plain action. */
static void t_combo_forward_release_after_func_lifts(void){
    combo_latch_t l; combo_latch_init(&l);
    struct combo_decision p = combo_dispatch(&l, 1, COMBO_BTN_FWD, 1);
    assert(p.consumed==1);
    struct combo_decision r = combo_dispatch(&l, 0, COMBO_BTN_FWD, 0);  /* •• already up */
    assert(r.consumed==1 && r.action==COMBO_NONE);
    assert(l.latched == 0);   /* latch cleared on swallow */
}
/* Reverse ordering: a PLAIN press (func NOT held) then •• pressed then release
 * while func_down is now 1. The press was NOT consumed (latch clear), so the
 * release must NOT be swallowed - the plain button keeps its own Note-On/Off
 * pairing. This is the case gating on func_down at release breaks. */
static void t_combo_reverse_plain_press_then_func(void){
    combo_latch_t l; combo_latch_init(&l);
    struct combo_decision p = combo_dispatch(&l, 0, COMBO_BTN_FWD, 1);  /* plain press */
    assert(p.consumed==0 && l.latched==0);
    struct combo_decision r = combo_dispatch(&l, 1, COMBO_BTN_FWD, 0);  /* •• now down */
    assert(r.consumed==0);   /* release routes normally, pairs the plain press */
}
/* Two different combo buttons held under one •• hold latch independently. */
static void t_combo_latch_is_per_index(void){
    combo_latch_t l; combo_latch_init(&l);
    combo_dispatch(&l, 1, COMBO_BTN_FWD, 1);
    combo_dispatch(&l, 1, COMBO_BTN_VOLUP, 1);
    struct combo_decision rf = combo_dispatch(&l, 1, COMBO_BTN_FWD, 0);
    assert(rf.consumed==1);
    struct combo_decision rv = combo_dispatch(&l, 0, COMBO_BTN_VOLUP, 0);  /* other order */
    assert(rv.consumed==1 && l.latched==0);
}

int main(void)
{
    t_combo_press_maps_and_consumes();
    t_mode_toggle_and_single_fire();
    t_combo_forward_release_after_func_lifts();
    t_combo_reverse_plain_press_then_func();
    t_combo_latch_is_per_index();
    t_led_pattern_table();
    printf("all mode tests passed\n");
    return 0;
}
