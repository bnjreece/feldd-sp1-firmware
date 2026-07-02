#pragma once
#include <stdbool.h>
#include <stdint.h>

/*
 * led.h — unified 8-LED drive over hardware PWM.
 *
 * Replaces the raw nrf_gpio on/off LED writes scattered through main.c so every
 * LED runs at a global BRIGHTNESS instead of full-on. pwm2 = track row, pwm3 =
 * side/play row (see the CANONICAL LED map in sp1_board.h); active-high, ~1 kHz,
 * normal polarity, so duty% == perceived brightness.
 *
 * Index scheme (matches host_led_set / led_override.c and the feldd_leds `pwms`
 * list in app.overlay):
 *   idx 0..3 = SP1_TRACK_LED1..4 (track row)
 *   idx 4..7 = SP1_PLAY_LED1..4  (side/play row; idx 7 == SP1_LED1 == P1.13)
 *
 * "on"  drives the channel at the current brightness; "off" drives duty 0.
 */
#define LED_COUNT              8
#define LED_BRIGHTNESS_DEFAULT 50   /* percent — "half brightness" per the field request */
#define LED_BRIGHTNESS_FULL    100  /* boot parade + any deliberately full-bright cue    */

/* Bring up all 8 PWM channels (call once at boot before any led_* drive). Returns
 * 0 when every channel's PWM device is ready, -1 otherwise (drive becomes a no-op
 * so a PWM bring-up failure can't wedge the boot path). */
int  led_init(void);
bool led_ready(void);

/* Drive one LED by index (0..7) or by its SP1_* GPIO pin macro. led_pin() is the
 * drop-in for nrf_gpio_pin_set/clear at the existing call sites; a non-LED pin is
 * ignored. Both apply the current global brightness. */
void led_idx(int idx, bool on);
void led_pin(uint32_t pin, bool on);

/* Global brightness, 0..100 (clamped). Re-applies immediately to all lit LEDs, so
 * the boot parade can bump to LED_BRIGHTNESS_FULL and restore afterward. */
void led_set_brightness(int pct);
int  led_get_brightness(void);
