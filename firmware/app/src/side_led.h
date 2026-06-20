#ifndef SIDE_LED_H
#define SIDE_LED_H

/* Pure, host-testable SIDE-row (SP1_PLAY_LED1..4) LED render for the feldd LED
 * redesign (spec 2026-06-19-feldd-led-redesign-side-layer-design.md §3, with the
 * BENJAMIN-APPROVED overrides: GLOBAL layer, "calm middle" mode-flash). It is
 * split out of main.c exactly like dial_track_pattern is (dial.c / test_led_dial.c)
 * so the two side-row writers can be host-asserted without GPIO.
 *
 * The side row has TWO writers per tick, arbitrated by a single non-blocking
 * deadline (mode_flash_ticks), priced exactly like the proven front-row
 * dial_confirm_ticks countdown:
 *
 *   1. MODE-FLASH (transient) — active while flash_ticks > 0. Shows the NEW mode's
 *      pattern (mode_led_pattern: MIDI = LEDs 1+4, KEYBOARD = LEDs 2+3) as the
 *      "calm middle": a single solid PULSE for the whole window (default), or a
 *      slow BLINK toggled on (tick/12)&1 (A/B style, MODE_FLASH_STYLE).
 *   2. LAYER rest (permanent) — the default: exactly ONE side LED lit at the
 *      layer position (natural ordering: layer 0 -> SP1_PLAY_LED1, ...), NEVER a
 *      bar. The layer is GLOBAL (gesture_layer()), unchanged across a mode switch,
 *      so after the flash the same layer LED re-asserts.
 *
 * Time is in SCAN TICKS (~8 ms), never wall-clock, so the same translation unit
 * links into both the firmware and the host test (it links mode.c alongside it for
 * mode_led_pattern, just as test_led_dial links dial.c). */

/* Render the side row for this tick into out[0..3] (each 0/1 for SP1_PLAY_LED i).
 *
 *   layer       : the current GLOBAL layer index (0..3). One LED lit at this
 *                 position at rest. Natural layer-to-pin ordering: 0 -> LED1.
 *   flash_ticks : the mode-flash deadline countdown. > 0 -> MODE-FLASH owns the
 *                 row this tick; 0 -> LAYER rest. The caller decrements it, NOT
 *                 this helper (pure, no state), mirroring dial_confirm_ticks.
 *   flash_mode  : the mode captured at the switch (MODE_MIDI/MODE_KEYBOARD); the
 *                 pattern source for the flash. Ignored when flash_ticks == 0.
 *   flash_blink : MODE_FLASH_STYLE — 0 = PULSE (solid pattern the whole window;
 *                 the calm default), non-0 = BLINK (toggle the pattern on/off on
 *                 the (tick/12)&1 phase). Ignored when flash_ticks == 0.
 *   tick        : the current scan tick, used ONLY for the BLINK phase.
 *
 * Always writes out[0..3] only. Output is strictly 0/1. */
void side_led_pattern(unsigned char layer, unsigned int flash_ticks,
                      unsigned char flash_mode, int flash_blink,
                      unsigned char tick, unsigned char out[4]);

#endif /* SIDE_LED_H */
