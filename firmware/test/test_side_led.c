#include <assert.h>
#include <stdio.h>
#include "../src/side_led.h"
#include "../src/mode.h"

/* Host tests for the pure SIDE-row (SP1_PLAY_LED1..4) render of the feldd LED
 * redesign (spec 2026-06-19-feldd-led-redesign-side-layer-design.md §3, with the
 * BENJAMIN-APPROVED overrides: GLOBAL layer + "calm middle" mode-flash):
 *
 *   LAYER rest (flash_ticks == 0) = exactly ONE side LED lit at the layer
 *                                   position (natural ordering, layer 0 -> LED1),
 *                                   NEVER a bar. Tick-independent.
 *   MODE-FLASH (flash_ticks > 0)  = the captured mode's pattern (mode_led_pattern:
 *                                   MIDI = LEDs 1+4 -> {1,0,0,1}; KEYBOARD = 2+3
 *                                   -> {0,1,1,0}). PULSE (flash_blink == 0) holds
 *                                   it solid the whole window; BLINK toggles it
 *                                   on/off on (tick/12)&1.
 *
 * side_led_pattern() is pure C99 (no Zephyr); it links mode.c alongside it for
 * mode_led_pattern, exactly the way test_led_dial links dial.c. Time is in scan
 * ticks, so the same translation unit links into firmware and this host test. */

/* The blink phase used by the helper under BLINK style: 1 = LIT, 0 = dark.
 * Mirrors dial.c's (tick/12)&1 and main.c's charge/profile cue. */
static int blink_on(unsigned char tick) { return (tick / 12) & 1; }

/* ----------------------------------------------------------------------------
 * 1. LAYER rest: exactly one LED at the layer position, never a bar. The spec
 *    examples: layer 0 -> {1,0,0,0}; layer 1 -> {0,1,0,0}; layer 3 -> {0,0,0,1}.
 *    flash_ticks == 0 so this is the resting case, tick-independent.
 * --------------------------------------------------------------------------*/
static void test_layer0_first_led(void) {
    for (unsigned char tick = 0; tick < 48; tick++) {
        unsigned char out[4];
        side_led_pattern(0, 0, MODE_MIDI, 0, tick, out);
        assert(out[0] == 1 && out[1] == 0 && out[2] == 0 && out[3] == 0);
    }
}

static void test_layer1_second_led(void) {
    for (unsigned char tick = 0; tick < 48; tick++) {
        unsigned char out[4];
        side_led_pattern(1, 0, MODE_MIDI, 0, tick, out);
        assert(out[0] == 0 && out[1] == 1 && out[2] == 0 && out[3] == 0);
    }
}

static void test_layer3_fourth_led(void) {
    for (unsigned char tick = 0; tick < 48; tick++) {
        unsigned char out[4];
        side_led_pattern(3, 0, MODE_MIDI, 0, tick, out);
        assert(out[0] == 0 && out[1] == 0 && out[2] == 0 && out[3] == 1);
    }
}

/* Every layer 0..3 lights exactly ONE LED, at its own position (never a bar). */
static void test_layer_is_single_led_natural_order(void) {
    for (unsigned char layer = 0; layer < 4; layer++) {
        unsigned char out[4];
        side_led_pattern(layer, 0, MODE_MIDI, 0, /*tick=*/7, out);
        int lit_count = 0;
        for (int i = 0; i < 4; i++) {
            assert(out[i] == (i == layer ? 1 : 0));   /* natural layer->pin */
            if (out[i]) lit_count++;
        }
        assert(lit_count == 1);                        /* one LED, never a bar */
    }
}

/* ----------------------------------------------------------------------------
 * 2. MODE-FLASH PULSE (flash_blink == 0): the mode pattern, SOLID at every tick
 *    for the whole window. MIDI -> the 1+4 pattern; KEYBOARD -> the 2+3 pattern.
 * --------------------------------------------------------------------------*/
static void test_flash_pulse_midi_is_1and4_solid(void) {
    for (unsigned char tick = 0; tick < 48; tick++) {
        unsigned char out[4];
        side_led_pattern(/*layer=*/2, /*flash_ticks=*/150, MODE_MIDI,
                         /*flash_blink=*/0, tick, out);
        /* MIDI = LEDs 1+4 = bits 0,3 -> {1,0,0,1}, solid regardless of tick */
        assert(out[0] == 1 && out[1] == 0 && out[2] == 0 && out[3] == 1);
    }
}

static void test_flash_pulse_keyboard_is_2and3_solid(void) {
    for (unsigned char tick = 0; tick < 48; tick++) {
        unsigned char out[4];
        side_led_pattern(/*layer=*/0, /*flash_ticks=*/150, MODE_KEYBOARD,
                         /*flash_blink=*/0, tick, out);
        /* KEYBOARD = LEDs 2+3 = bits 1,2 -> {0,1,1,0}, solid regardless of tick */
        assert(out[0] == 0 && out[1] == 1 && out[2] == 1 && out[3] == 0);
    }
}

/* ----------------------------------------------------------------------------
 * 3. MODE-FLASH BLINK (flash_blink != 0): the mode pattern where (tick/12)&1==1,
 *    all-off where ==0. Verify both phases at explicit checkpoints and over a run.
 * --------------------------------------------------------------------------*/
static void test_flash_blink_midi_toggles_with_phase(void) {
    int saw_lit = 0, saw_dark = 0;
    for (unsigned char tick = 0; tick < 48; tick++) {
        unsigned char out[4];
        side_led_pattern(/*layer=*/1, /*flash_ticks=*/150, MODE_MIDI,
                         /*flash_blink=*/1, tick, out);
        if (blink_on(tick)) {
            /* phase 1: the 1+4 pattern shows */
            assert(out[0] == 1 && out[1] == 0 && out[2] == 0 && out[3] == 1);
            saw_lit = 1;
        } else {
            /* phase 0: all off */
            assert(out[0] == 0 && out[1] == 0 && out[2] == 0 && out[3] == 0);
            saw_dark = 1;
        }
    }
    assert(saw_lit && saw_dark);   /* really toggles across the window */
}

/* Explicit phase checkpoints (mirrors test_led_dial's blink checkpoints): dark at
 * tick 0/11 (phase 0), pattern at tick 12/23 (phase 1), dark again at tick 24. */
static void test_flash_blink_phase_checkpoints(void) {
    unsigned char out[4];
    side_led_pattern(0, 150, MODE_KEYBOARD, 1, 0, out);
    assert(out[0] == 0 && out[1] == 0 && out[2] == 0 && out[3] == 0);  /* phase 0 */
    side_led_pattern(0, 150, MODE_KEYBOARD, 1, 11, out);
    assert(out[0] == 0 && out[1] == 0 && out[2] == 0 && out[3] == 0);  /* phase 0 */
    side_led_pattern(0, 150, MODE_KEYBOARD, 1, 12, out);
    assert(out[0] == 0 && out[1] == 1 && out[2] == 1 && out[3] == 0);  /* phase 1: 2+3 */
    side_led_pattern(0, 150, MODE_KEYBOARD, 1, 23, out);
    assert(out[0] == 0 && out[1] == 1 && out[2] == 1 && out[3] == 0);  /* phase 1 */
    side_led_pattern(0, 150, MODE_KEYBOARD, 1, 24, out);
    assert(out[0] == 0 && out[1] == 0 && out[2] == 0 && out[3] == 0);  /* phase 0 */
}

/* ----------------------------------------------------------------------------
 * 4. flash_ticks == 0 -> the LAYER pattern regardless of flash_mode/flash_blink
 *    (the deadline, not the mode, decides who owns the row).
 * --------------------------------------------------------------------------*/
static void test_flash_ticks_zero_is_always_layer(void) {
    for (unsigned char layer = 0; layer < 4; layer++) {
        for (int mode = MODE_MIDI; mode <= MODE_KEYBOARD; mode++) {
            for (int blink = 0; blink <= 1; blink++) {
                for (unsigned char tick = 0; tick < 36; tick++) {
                    unsigned char out[4];
                    side_led_pattern(layer, /*flash_ticks=*/0,
                                     (unsigned char)mode, blink, tick, out);
                    for (int i = 0; i < 4; i++) {
                        assert(out[i] == (i == layer ? 1 : 0));
                    }
                }
            }
        }
    }
}

/* ----------------------------------------------------------------------------
 * 5. The flash render matches mode_led_pattern's bit order EXACTLY (the spec's
 *    "confirm it matches mode_led_pattern bit order"): for each mode, PULSE
 *    out[i] == (mode_led_pattern(mode) >> i) & 1, for every defined + reserved
 *    + clamped mode. This is the load-bearing desync guard.
 * --------------------------------------------------------------------------*/
static void test_flash_matches_mode_led_pattern_bit_order(void) {
    int modes[] = { MODE_MIDI, MODE_KEYBOARD, 2, 3, 99 /* clamps to MIDI */ };
    for (unsigned m = 0; m < sizeof(modes) / sizeof(modes[0]); m++) {
        uint8_t pat = mode_led_pattern(modes[m]);
        unsigned char out[4];
        /* PULSE: solid pattern, so out is the raw bit unpack at any tick. */
        side_led_pattern(/*layer=*/2, /*flash_ticks=*/150, (unsigned char)modes[m],
                         /*flash_blink=*/0, /*tick=*/5, out);
        for (int i = 0; i < 4; i++) {
            assert(out[i] == (unsigned char)((pat >> i) & 1u));
        }
    }
}

/* ----------------------------------------------------------------------------
 * 6. Output is strictly 0/1 for every (layer, flash_ticks, mode, blink, tick)
 *    combination exercised (no stray values leak into the GPIO render path).
 * --------------------------------------------------------------------------*/
static void test_output_is_binary(void) {
    int modes[] = { MODE_MIDI, MODE_KEYBOARD };
    for (unsigned char layer = 0; layer < 4; layer++) {
        for (unsigned int ft = 0; ft <= 150; ft += 50) {
            for (unsigned m = 0; m < sizeof(modes) / sizeof(modes[0]); m++) {
                for (int blink = 0; blink <= 1; blink++) {
                    for (unsigned char tick = 0; tick < 30; tick++) {
                        unsigned char out[4];
                        side_led_pattern(layer, ft, (unsigned char)modes[m],
                                         blink, tick, out);
                        for (int i = 0; i < 4; i++) {
                            assert(out[i] == 0 || out[i] == 1);
                        }
                    }
                }
            }
        }
    }
}

int main(void) {
    test_layer0_first_led();
    test_layer1_second_led();
    test_layer3_fourth_led();
    test_layer_is_single_led_natural_order();
    test_flash_pulse_midi_is_1and4_solid();
    test_flash_pulse_keyboard_is_2and3_solid();
    test_flash_blink_midi_toggles_with_phase();
    test_flash_blink_phase_checkpoints();
    test_flash_ticks_zero_is_always_layer();
    test_flash_matches_mode_led_pattern_bit_order();
    test_output_is_binary();
    printf("all side led tests passed\n");
    return 0;
}
