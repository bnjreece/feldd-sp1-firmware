#include <assert.h>
#include <stdio.h>
#include "../src/control_logic.h"

static void test_fader_to_cc_quantizes_and_suppresses_jitter(void) {
    fader_t f = {0};
    // 12-bit 0..4095 -> 7-bit 0..127, first read always reports
    assert(fader_update(&f, 0) == 0);          // CC value 0
    assert(fader_update(&f, 4095) == 127);     // full scale
    assert(fader_update(&f, 4095 - 8) == -1);  // same CC step: no send
    assert(fader_update(&f, 2048) == 64);      // big move: sends ~center
    assert(fader_update(&f, 2056) == -1);      // jitter suppressed
    assert(fader_update(&f, 2032) == 63);      // delta == FADER_HYSTERESIS exactly: sends
}

static void test_fader_rails_reachable_after_boundary_jitter(void) {
    fader_t f = {0};
    assert(fader_update(&f, 33) == 1);     // first read reports CC 1
    assert(fader_update(&f, 31) == 0);     // low rail band: emits CC 0 despite jitter window
    assert(fader_update(&f, 0) == -1);     // same CC step: duplicate suppressed
    assert(fader_update(&f, 4095) == 127); // top rail reachable from anywhere
}

static void test_fader_rail_bands_bypass_jitter_window(void) {
    // an ADC limited by pot end resistance may never read true 0/4095;
    // any reading inside the top/bottom CC step must still reach the extreme
    fader_t f = {0};
    assert(fader_update(&f, 4060) == 126);  // resting just below the top step
    assert(fader_update(&f, 4070) == 127);  // delta 10 < hysteresis, but in top rail band
    assert(fader_update(&f, 4085) == -1);   // same CC step: duplicate suppressed
    fader_t g = {0};
    assert(fader_update(&g, 35) == 1);      // resting just above the bottom step
    assert(fader_update(&g, 25) == 0);      // delta 10 < hysteresis, but in bottom rail band
    assert(fader_update(&g, 5) == -1);      // same CC step: duplicate suppressed
}

static void test_fader_reinit_reemits_unchanged_raw(void) {
    // After a profile switch the firmware re-arms every fader by zeroing
    // .initialized (faders_rearm in main.c). A re-armed fader must re-emit its
    // current value even though the raw code has not moved, so the new
    // profile's CC/range/curve mapping reaches the host/OP-XY immediately.
    fader_t f = {0};
    assert(fader_update(&f, 2048) == 64);   // first read seeds + emits CC 64
    assert(fader_update(&f, 2048) == -1);   // unchanged raw: suppressed
    f.initialized = 0;                      // simulate faders_rearm() on switch
    assert(fader_update(&f, 2048) == 64);   // re-armed: re-emits despite no move
    assert(fader_update(&f, 2048) == -1);   // and suppresses again afterward
}

static void test_ladder_decode_matches_first_level_within_tolerance(void) {
    // example calibration: idle=4095, then per-button plateaus
    static const uint16_t levels[] = {3500, 2800, 2100, 1400, 700};
    assert(ladder_decode(4095, levels, 5, 150) == -1);   // idle: no button
    assert(ladder_decode(3520, levels, 5, 150) == 0);    // button 0
    assert(ladder_decode(3650, levels, 5, 150) == 0);    // d == tol exactly: matches
    assert(ladder_decode(690,  levels, 5, 150) == 4);    // button 4
    assert(ladder_decode(3100, levels, 5, 150) == -1);   // between plateaus: reject
}

int main(void) {
    test_fader_to_cc_quantizes_and_suppresses_jitter();
    test_fader_rails_reachable_after_boundary_jitter();
    test_fader_rail_bands_bypass_jitter_window();
    test_fader_reinit_reemits_unchanged_raw();
    test_ladder_decode_matches_first_level_within_tolerance();
    printf("all control_logic tests passed\n");
    return 0;
}
