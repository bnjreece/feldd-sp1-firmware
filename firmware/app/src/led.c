/*
 * led.c — 8-LED drive over hardware PWM (see led.h).
 *
 * Wraps the two nRF PWM peripherals (pwm2 = track row, pwm3 = side/play row,
 * enabled + pin-mapped in app.overlay) behind a tiny on/off + global-brightness
 * API. The nRF PWM runs the duty autonomously (no loop cost, no software-PWM
 * flicker), so "half brightness" is just a smaller pulse width.
 *
 * The channel order in spec[]/pin_of[] MUST match the feldd_leds `pwms` list in
 * app.overlay: idx 0..3 = SP1_TRACK_LED1..4, idx 4..7 = SP1_PLAY_LED1..4.
 */
#include "led.h"
#include "sp1_board.h"
#include <zephyr/drivers/pwm.h>
#include <zephyr/devicetree.h>

/* Order MUST match the feldd_leds children in app.overlay:
 * idx 0..3 = SP1_TRACK_LED1..4, idx 4..7 = SP1_PLAY_LED1..4. */
static const struct pwm_dt_spec spec[LED_COUNT] = {
    PWM_DT_SPEC_GET(DT_NODELABEL(fled_t1)),
    PWM_DT_SPEC_GET(DT_NODELABEL(fled_t2)),
    PWM_DT_SPEC_GET(DT_NODELABEL(fled_t3)),
    PWM_DT_SPEC_GET(DT_NODELABEL(fled_t4)),
    PWM_DT_SPEC_GET(DT_NODELABEL(fled_p1)),
    PWM_DT_SPEC_GET(DT_NODELABEL(fled_p2)),
    PWM_DT_SPEC_GET(DT_NODELABEL(fled_p3)),
    PWM_DT_SPEC_GET(DT_NODELABEL(fled_p4)),
};

/* idx -> SP1_* pin, so led_pin() can stay a drop-in at the existing call sites.
 * SP1_PLAY_LED4 aliases SP1_LED1 (both P1.13), so led_pin(SP1_LED1, ...) resolves
 * to idx 7 automatically. */
static const uint32_t pin_of[LED_COUNT] = {
    SP1_TRACK_LED1, SP1_TRACK_LED2, SP1_TRACK_LED3, SP1_TRACK_LED4,
    SP1_PLAY_LED1,  SP1_PLAY_LED2,  SP1_PLAY_LED3,  SP1_PLAY_LED4,
};

static bool ready;
static bool state[LED_COUNT];
static int  brightness = LED_BRIGHTNESS_DEFAULT;

static void apply(int i)
{
    if (!ready) {
        return;
    }
    uint32_t period = spec[i].period;
    uint32_t pulse  = state[i]
        ? (uint32_t)(((uint64_t)period * (uint32_t)brightness) / 100u)
        : 0u;
    (void)pwm_set_pulse_dt(&spec[i], pulse);
}

int led_init(void)
{
    ready = true;
    for (int i = 0; i < LED_COUNT; i++) {
        if (!pwm_is_ready_dt(&spec[i])) {
            ready = false;
        }
    }
    /* Start all off (also proves each channel by writing duty 0). */
    for (int i = 0; i < LED_COUNT; i++) {
        state[i] = false;
        apply(i);
    }
    return ready ? 0 : -1;
}

bool led_ready(void)
{
    return ready;
}

void led_idx(int i, bool on)
{
    if (i < 0 || i >= LED_COUNT) {
        return;
    }
    state[i] = on;
    apply(i);
}

void led_pin(uint32_t pin, bool on)
{
    for (int i = 0; i < LED_COUNT; i++) {
        if (pin_of[i] == pin) {
            led_idx(i, on);
            return;
        }
    }
    /* Not one of the 8 LED pins — ignore, so callers that pass through non-LED
     * pins stay harmless. */
}

void led_set_brightness(int pct)
{
    if (pct < 0) {
        pct = 0;
    } else if (pct > 100) {
        pct = 100;
    }
    brightness = pct;
    for (int i = 0; i < LED_COUNT; i++) {
        apply(i);
    }
}

int led_get_brightness(void)
{
    return brightness;
}
