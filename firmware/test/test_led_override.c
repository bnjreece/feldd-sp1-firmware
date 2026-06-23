/* test_led_override.c — pure host-LED override mask logic (feldd-cc led verb).
 * The module is freestanding (no Zephyr/GPIO), so the bit-mask + active flag are
 * fully host-testable. Globals are static (no reset API), so each test sets its
 * own precondition; the fresh-state test runs first to see the zero-init values. */
#include <assert.h>
#include <stdio.h>
#include "../src/led_override.h"

static void test_fresh_state_inactive_mask_zero(void) {
    assert(host_led_active() == false);   /* zero-init: no host owns the LEDs */
    assert(host_led_get_mask() == 0x00);
}

static void test_set_single_led_engages_and_toggles_bit(void) {
    host_led_set(2, true);
    assert(host_led_active() == true);        /* set engages the override */
    assert(host_led_get_mask() == 0x04);      /* bit 2 */
    host_led_set(2, false);
    assert(host_led_get_mask() == 0x00);      /* bit cleared */
    assert(host_led_active() == true);        /* still engaged (release is separate) */
}

static void test_out_of_range_index_ignored(void) {
    host_led_mask(0x12);                       /* known state */
    host_led_set(8, true);                     /* ix > 7: ignored, no change */
    assert(host_led_get_mask() == 0x12);
}

static void test_mask_replaces_whole_byte_and_engages(void) {
    host_led_release();                        /* start inactive */
    host_led_mask(0xA5);
    assert(host_led_get_mask() == 0xA5);
    assert(host_led_active() == true);
}

static void test_release_clears_active(void) {
    host_led_mask(0xFF);                       /* engaged */
    host_led_release();
    assert(host_led_active() == false);        /* host handed the LEDs back */
}

int main(void) {
    test_fresh_state_inactive_mask_zero();
    test_set_single_led_engages_and_toggles_bit();
    test_out_of_range_index_ignored();
    test_mask_replaces_whole_byte_and_engages();
    test_release_clears_active();
    printf("all led_override tests passed\n");
    return 0;
}
