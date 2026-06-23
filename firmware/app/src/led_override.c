/*
 * led_override.c — host-LED override state (see led_override.h).
 *
 * PURE: no Zephyr, no GPIO. The bit-mask IS the LED truth while active; main.c's
 * render maps bit i -> LED i. Single translation unit, links into both the
 * firmware and the host test (test_led_override.c).
 */
#include "led_override.h"

static uint8_t g_host_led_mask;    /* bit i (1<<i) = LED i on; 0..7 = track0..3, play0..3 */
static bool    g_host_led_active;  /* set/mask latch this on; release clears it */

void host_led_set(uint8_t ix, bool on)
{
    if (ix > 7) {
        return;
    }
    if (on) {
        g_host_led_mask |= (uint8_t)(1u << ix);
    } else {
        g_host_led_mask &= (uint8_t)~(1u << ix);
    }
    g_host_led_active = true;
}

void host_led_mask(uint8_t mask)
{
    g_host_led_mask   = mask;
    g_host_led_active = true;
}

void host_led_release(void)
{
    g_host_led_active = false;
    /* Leave g_host_led_mask as-is: irrelevant while inactive, and a re-engage via
     * host_led_set() resumes from the last mask (the host's mental model). */
}

bool host_led_active(void)
{
    return g_host_led_active;
}

uint8_t host_led_get_mask(void)
{
    return g_host_led_mask;
}
