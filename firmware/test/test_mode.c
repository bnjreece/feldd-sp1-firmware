#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "mode.h"

/* A FWD press while the modifier (PLAY) is held -> set KEYBOARD + consume the edge. */
static void t_fwd_press_with_func_sets_keyboard(void)
{
    struct mode_decision d = mode_decide(/*modifier_held=*/1, /*idx=*/7, /*pressed=*/1);
    assert(d.set == 1);
    assert(d.which == MODE_KEYBOARD);
    assert(d.consumed == 1);
}

/* A RWD press while the modifier (PLAY) is held -> set MIDI + consume the edge. */
static void t_rwd_press_with_func_sets_midi(void)
{
    struct mode_decision d = mode_decide(1, 8, 1);
    assert(d.set == 1);
    assert(d.which == MODE_MIDI);
    assert(d.consumed == 1);
}

/* FWD/RWD with the modifier NOT held -> no set, NOT consumed (normal mapping must run). */
static void t_fwd_without_func_is_passthrough(void)
{
    struct mode_decision d = mode_decide(0, 7, 1);
    assert(d.set == 0);
    assert(d.consumed == 0);
    struct mode_decision r = mode_decide(0, 8, 1);
    assert(r.set == 0);
    assert(r.consumed == 0);
}

/* A RELEASE edge of FWD/RWD while the modifier held: no set, but still consumed
 * so the release of the selector button never types/maps. */
static void t_release_with_func_is_consumed_not_set(void)
{
    struct mode_decision d = mode_decide(1, 7, 0);
    assert(d.set == 0);
    assert(d.consumed == 1);
}

/* A non-FWD/RWD button while the modifier held -> passthrough (modifier held +
 * other ladder button is not a mode gesture). */
static void t_other_button_with_func_is_passthrough(void)
{
    for (int idx = 0; idx <= 6; idx++) {
        struct mode_decision d = mode_decide(1, idx, 1);
        assert(d.set == 0);
        assert(d.consumed == 0);
    }
}

/* The consume contract (the load-bearing case): a FWD/RWD tap while the modifier
 * (PLAY-held) is engaged reports both `set` and `consumed`, so the caller persists
 * the mode AND swallows the edge (no map/type) + disarms the shift detector.
 * mode_decide is the single source for that signal. */
static void t_tap_reports_both_set_and_consumed(void)
{
    struct mode_decision d = mode_decide(1, 7, 1);
    assert(d.set && d.consumed);   /* caller: librarian_set_mode + gesture_disarm */
}

/* The N-mode LED pattern table (renders on the PLAY row, SP1_PLAY_LED1..4 per
 * spec §7.4 — USER-APPROVED driving the 3 spare play LEDs). MIDI lights 1+4,
 * KEYBOARD lights 2+3. */
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

int main(void)
{
    t_fwd_press_with_func_sets_keyboard();
    t_rwd_press_with_func_sets_midi();
    t_fwd_without_func_is_passthrough();
    t_release_with_func_is_consumed_not_set();
    t_other_button_with_func_is_passthrough();
    t_tap_reports_both_set_and_consumed();
    t_led_pattern_table();
    printf("all mode tests passed\n");
    return 0;
}
