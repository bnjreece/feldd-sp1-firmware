#include <assert.h>
#include <stdio.h>
#include "../src/dial.h"

/* Host tests for the pure dial COUNT -> TRACK-ROW LED pattern mapping (design
 * spec §8 "LED feedback — TRACK ROW ONLY"):
 *   counts 1-4 = that many track LEDs SOLID, building left->right (1 = index 0;
 *                4 = all four).
 *   counts 5-8 = all four SOLID + BLINK the (count-4)th LED (index count-5),
 *                i.e. 5 blinks index 0 ... 8 blinks index 3. The blink toggles
 *                on `tick` using the same ~12-tick half-period as main.c's charge
 *                / profile cue: (tick/12) & 1.
 *   count 0    = all off (idle; caller falls back to layer-rest).
 *
 * dial_track_pattern() is pure C99 (no Zephyr), time is in scan ticks, so the
 * exact same translation unit links into both firmware and this host test. */

/* The blink phase used by the helper: 1 = the blinking LED is currently LIT,
 * 0 = currently dark. Mirrors main.c:319's ((++blink / 12u) & 1u). */
static int blink_on(unsigned char tick) { return (tick / 12) & 1; }

/* ----------------------------------------------------------------------------
 * 1. count 1 -> {1,0,0,0}: exactly one solid LED, index 0. Tick-independent
 *    (no blink in the 1-4 solid range), so it must read identical at any tick.
 * --------------------------------------------------------------------------*/
static void test_count1_one_led(void) {
    for (unsigned char tick = 0; tick < 48; tick++) {
        unsigned char out[4];
        dial_track_pattern(1, tick, out);
        assert(out[0] == 1);
        assert(out[1] == 0);
        assert(out[2] == 0);
        assert(out[3] == 0);
    }
}

/* ----------------------------------------------------------------------------
 * 2. counts 1-4 build left->right, all solid (tick-independent). 2 = {1,1,0,0},
 *    3 = {1,1,1,0}, 4 = {1,1,1,1}.
 * --------------------------------------------------------------------------*/
static void test_counts_1_to_4_build_left_to_right(void) {
    for (unsigned char count = 1; count <= 4; count++) {
        for (unsigned char tick = 0; tick < 48; tick++) {
            unsigned char out[4];
            dial_track_pattern(count, tick, out);
            for (int i = 0; i < 4; i++) {
                /* LEDs 0..count-1 are solid on; the rest are off */
                assert(out[i] == (i < count ? 1 : 0));
            }
        }
    }
}

/* Explicit spec example: count 4 = {1,1,1,1}. */
static void test_count4_all_four(void) {
    unsigned char out[4];
    dial_track_pattern(4, 0, out);
    assert(out[0] == 1 && out[1] == 1 && out[2] == 1 && out[3] == 1);
}

/* ----------------------------------------------------------------------------
 * 3. count 5 -> all four ON, with index 0 the blinker: ON where (tick/12)&1 is 1,
 *    OFF otherwise; indices 1..3 are always solid. Verify the toggle across ticks.
 * --------------------------------------------------------------------------*/
static void test_count5_index0_toggles(void) {
    int saw_lit = 0, saw_dark = 0;
    for (unsigned char tick = 0; tick < 48; tick++) {
        unsigned char out[4];
        dial_track_pattern(5, tick, out);
        /* the three non-blinking LEDs are always solid */
        assert(out[1] == 1);
        assert(out[2] == 1);
        assert(out[3] == 1);
        /* the blinking LED (index 0) follows the tick phase exactly */
        assert(out[0] == (blink_on(tick) ? 1 : 0));
        if (out[0]) saw_lit = 1; else saw_dark = 1;
    }
    /* prove it really toggles (both phases observed over 48 ticks = 4 periods) */
    assert(saw_lit && saw_dark);
}

/* Spec example checkpoints for count 5: dark at tick 0 (phase 0), lit at tick 12
 * (phase 1), dark again at tick 24. */
static void test_count5_blink_phase_checkpoints(void) {
    unsigned char out[4];
    dial_track_pattern(5, 0, out);   assert(out[0] == 0);  /* (0/12)&1 = 0 */
    dial_track_pattern(5, 11, out);  assert(out[0] == 0);  /* still phase 0 */
    dial_track_pattern(5, 12, out);  assert(out[0] == 1);  /* (12/12)&1 = 1 */
    dial_track_pattern(5, 23, out);  assert(out[0] == 1);  /* still phase 1 */
    dial_track_pattern(5, 24, out);  assert(out[0] == 0);  /* (24/12)&1 = 0 */
}

/* ----------------------------------------------------------------------------
 * 4. counts 5-8 each blink the (count-4)th LED (index count-5) and hold the
 *    other three solid. 6 -> index 1, 7 -> index 2, 8 -> index 3.
 * --------------------------------------------------------------------------*/
static void test_counts_5_to_8_blink_correct_index(void) {
    for (unsigned char count = 5; count <= 8; count++) {
        int blink_idx = count - 5;
        int saw_lit = 0, saw_dark = 0;
        for (unsigned char tick = 0; tick < 48; tick++) {
            unsigned char out[4];
            dial_track_pattern(count, tick, out);
            for (int i = 0; i < 4; i++) {
                if (i == blink_idx) {
                    assert(out[i] == (blink_on(tick) ? 1 : 0));
                    if (out[i]) saw_lit = 1; else saw_dark = 1;
                } else {
                    assert(out[i] == 1);  /* the other three stay solid */
                }
            }
        }
        assert(saw_lit && saw_dark);  /* the chosen index really toggles */
    }
}

/* Explicit spec example: count 8 = all on with index 3 toggling. */
static void test_count8_index3_toggles(void) {
    /* phase 1 (tick 12): all four lit */
    unsigned char out[4];
    dial_track_pattern(8, 12, out);
    assert(out[0] == 1 && out[1] == 1 && out[2] == 1 && out[3] == 1);
    /* phase 0 (tick 0): index 3 dark, the other three solid */
    dial_track_pattern(8, 0, out);
    assert(out[0] == 1 && out[1] == 1 && out[2] == 1 && out[3] == 0);
}

/* ----------------------------------------------------------------------------
 * 5. count 0 -> all off, at every tick (idle; caller falls back to layer-rest).
 * --------------------------------------------------------------------------*/
static void test_count0_all_off(void) {
    for (unsigned char tick = 0; tick < 48; tick++) {
        unsigned char out[4];
        dial_track_pattern(0, tick, out);
        assert(out[0] == 0 && out[1] == 0 && out[2] == 0 && out[3] == 0);
    }
}

/* ----------------------------------------------------------------------------
 * 6. Output is strictly 0/1 for every legal count and a range of ticks (no
 *    stray values leak into the LED render path).
 * --------------------------------------------------------------------------*/
static void test_output_is_binary(void) {
    for (unsigned char count = 0; count <= DIAL_MAX_COUNT; count++) {
        for (unsigned char tick = 0; tick < 60; tick++) {
            unsigned char out[4];
            dial_track_pattern(count, tick, out);
            for (int i = 0; i < 4; i++) {
                assert(out[i] == 0 || out[i] == 1);
            }
        }
    }
}

int main(void) {
    test_count1_one_led();
    test_counts_1_to_4_build_left_to_right();
    test_count4_all_four();
    test_count5_index0_toggles();
    test_count5_blink_phase_checkpoints();
    test_counts_5_to_8_blink_correct_index();
    test_count8_index3_toggles();
    test_count0_all_off();
    test_output_is_binary();
    printf("all led dial tests passed\n");
    return 0;
}
