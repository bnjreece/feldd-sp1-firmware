#ifndef LED_OVERRIDE_H
#define LED_OVERRIDE_H
#include <stdint.h>
#include <stdbool.h>

/*
 * led_override — host-LED override state for the CDC "led" verb.
 *
 * PURE & freestanding (no Zephyr, no GPIO): just the override mask + active flag
 * and a tiny API, so the protocol layer can engage it from proto_handle while
 * staying host-testable, and main.c's per-tick LED render can consult it as the
 * single highest-priority writer. This is the "feldd as Claude Code console"
 * hook: a host program drives the 8 LEDs (4 track + 4 side/play) directly.
 *
 * LED index space (bit i, 0..7):
 *   0..3 = SP1_TRACK_LED1..4 (front/track row)
 *   4..7 = SP1_PLAY_LED1..4  (side/play row)
 *
 * Engaging (set or mask) latches host_led_active(); the normal feldd render is
 * untouched until then, and host_led_release() hands the LEDs back to it.
 */

/* Set a single LED (ix 0..7) on/off and ENGAGE host override. ix > 7 is ignored
 * (the protocol layer validates ix <= 7 before calling, this is belt-and-braces). */
void    host_led_set(uint8_t ix, bool on);

/* Replace the whole 8-LED mask (bit i = LED i on) and ENGAGE host override. */
void    host_led_mask(uint8_t mask);

/* Release the override: LEDs return to feldd's normal per-tick render. */
void    host_led_release(void);

/* True while a host currently owns the LEDs. */
bool    host_led_active(void);

/* The current host mask (only meaningful while host_led_active()). */
uint8_t host_led_get_mask(void);

#endif /* LED_OVERRIDE_H */
