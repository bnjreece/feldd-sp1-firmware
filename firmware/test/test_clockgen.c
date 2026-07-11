#include <assert.h>
#include <stdio.h>
#include "../src/clockgen.h"

static void test_tick_us(void) {
    /* 2500000 / 120 = 20833 (integer) */
    assert(clockgen_tick_us(120) == 20833);
    /* bpm below 40 clamps to 40: 2500000 / 40 = 62500 */
    assert(clockgen_tick_us(30) == 62500);
    assert(clockgen_tick_us(30) == clockgen_tick_us(40));
    /* bpm above 240 clamps to 240: 2500000 / 240 = 10416 */
    assert(clockgen_tick_us(300) == 10416);
    assert(clockgen_tick_us(300) == clockgen_tick_us(240));
    /* boundary values map straight through */
    assert(clockgen_tick_us(40) == 62500);
    assert(clockgen_tick_us(240) == 10416);
}

static void test_us_to_bpm(void) {
    assert(clockgen_us_to_bpm(20833) == 120);
    /* div0 guard */
    assert(clockgen_us_to_bpm(0) == 40);
    /* round-trip at the clamp edges */
    assert(clockgen_us_to_bpm(62500) == 40);
    assert(clockgen_us_to_bpm(10416) == 240);
    /* huge tick (slow) clamps up to 40; tiny tick (fast) clamps down to 240 */
    assert(clockgen_us_to_bpm(10000000) == 40);
    assert(clockgen_us_to_bpm(1) == 240);
}

static void test_fader_bpm(void) {
    /* v=0 -> min, v=127 -> max */
    assert(clockgen_fader_bpm(0, 40, 240) == 40);
    assert(clockgen_fader_bpm(127, 40, 240) == 240);
    /* midpoint near the average of min and max */
    uint16_t mid = clockgen_fader_bpm(64, 40, 240);
    uint16_t avg = (40 + 240) / 2;
    assert(mid >= avg - 2 && mid <= avg + 2);
    /* result never escapes [min,max] */
    assert(clockgen_fader_bpm(200, 40, 240) == 240); /* v out of 0..127 range */
    /* narrow range still endpoints */
    assert(clockgen_fader_bpm(0, 100, 130) == 100);
    assert(clockgen_fader_bpm(127, 100, 130) == 130);
}

static void test_tap_bpm(void) {
    struct clock_tap t;
    clock_tap_reset(&t);
    /* first tap: no prior -> 0 (need a 2nd) */
    assert(clock_tap_bpm(&t, 0) == 0);
    /* three taps 500000 us apart -> 120 BPM (60,000,000 / 500,000) */
    assert(clock_tap_bpm(&t, 500000) == 120);
    assert(clock_tap_bpm(&t, 1000000) == 120);
    /* a 4th tap after a > 2 s gap resets and returns 0 */
    assert(clock_tap_bpm(&t, 1000000 + 2000001) == 0);
    /* and a subsequent close tap re-establishes tempo from the reset point */
    assert(clock_tap_bpm(&t, 1000000 + 2000001 + 500000) == 120);
}

static void test_tap_rolling_average(void) {
    struct clock_tap t;
    clock_tap_reset(&t);
    assert(clock_tap_bpm(&t, 0) == 0);
    /* two 500000 intervals then two 400000 intervals: only last 4 kept.
     * timestamps: 0, 500000, 1000000, 1400000, 1800000
     * intervals:      500000, 500000,  400000,  400000  (avg 450000)
     * 60,000,000 / 450,000 = 133 */
    assert(clock_tap_bpm(&t, 500000) == 120);
    assert(clock_tap_bpm(&t, 1000000) == 120);
    clock_tap_bpm(&t, 1400000);
    assert(clock_tap_bpm(&t, 1800000) == 133);
}

int main(void) {
    test_tick_us();
    test_us_to_bpm();
    test_fader_bpm();
    test_tap_bpm();
    test_tap_rolling_average();
    printf("all clockgen tests passed\n");
    return 0;
}
