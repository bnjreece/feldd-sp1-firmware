#include <assert.h>
#include <stdio.h>
#include "../src/control_logic.h"

static void test_fader_rescales_to_full_0_127(void) {
    // SP-1 pots top out ~raw 3680, which a plain raw>>5 capped at CC 115.
    // raw_to_cc rescales the usable span so full travel reaches 0..127.
    // First read always reports.
    fader_t a = {0}; assert(fader_update(&a, 0)    == 0);    // bottom -> CC 0
    fader_t b = {0}; assert(fader_update(&b, 3680) == 127);  // physical max -> CC 127 (was 115)
    fader_t c = {0}; assert(fader_update(&c, 4095) == 127);  // ADC full scale -> CC 127
    fader_t d = {0}; assert(fader_update(&d, 1841) == 63);   // mid travel -> ~center
}

static void test_fader_suppresses_jitter_and_duplicates(void) {
    fader_t f = {0};
    assert(fader_update(&f, 1841) == 63);   // seed at CC 63
    assert(fader_update(&f, 1849) == -1);   // +8 raw, same CC 63: suppressed
    assert(fader_update(&f, 1830) == -1);   // -11 raw, same CC 63: suppressed
    assert(fader_update(&f, 2100) == 72);   // big move clears the step + window: emits
}

static void test_fader_hysteresis_boundary_emits(void) {
    fader_t f = {0};
    assert(fader_update(&f, 2084) == 72);   // seed at CC 72
    assert(fader_update(&f, 2100) == -1);   // +16 raw but still CC 72: duplicate-suppressed
    assert(fader_update(&f, 2068) == 71);   // -16 (== hysteresis) crossing to CC 71: emits
}

static void test_fader_top_rail_bypasses_jitter_window(void) {
    // resting just below the top, a sub-hysteresis nudge into the top dead-band
    // must still reach CC 127 (the rail band bypasses the jitter window)
    fader_t f = {0};
    assert(fader_update(&f, 3640) == 126);  // resting just below the top step
    assert(fader_update(&f, 3652) == 127);  // +12 (< hysteresis) but >= MAX rail: emits 127
    assert(fader_update(&f, 3800) == -1);   // same CC step (127): duplicate suppressed
}

static void test_fader_bottom_reachable_and_reinit_reemits(void) {
    fader_t f = {0};
    assert(fader_update(&f, 80) == 1);      // resting just above the bottom step (CC 1)
    assert(fader_update(&f, 30) == 0);      // into the bottom rail: emits CC 0
    // After a profile switch the firmware re-arms every fader by zeroing
    // .initialized (faders_rearm in main.c); a re-armed fader must re-emit its
    // current value even though the raw code has not moved.
    f.initialized = 0;                      // simulate faders_rearm() on switch
    assert(fader_update(&f, 30) == 0);      // re-armed: re-emits despite no move
    assert(fader_update(&f, 30) == -1);     // and suppresses again afterward
}

static void test_takeover_no_catch_when_at_target(void) {
    takeover_t t = {0};
    assert(takeover_arm(&t, 64, 64) == 0);   // already at target: nothing to catch
    assert(t.pending == 0);
    assert(takeover_step(&t, 64) == 1);      // not pending: emit normally
    assert(takeover_step(&t, 70) == 1);      // still not pending
}

static void test_takeover_catch_from_above(void) {
    takeover_t t = {0};
    assert(takeover_arm(&t, 100, 40) == 1);  // physical above the target -> catch
    assert(t.pending == 1 && t.from_above == 1);
    assert(takeover_step(&t, 90) == 0);      // still above: suppress
    assert(takeover_step(&t, 41) == 0);      // just above: suppress
    assert(takeover_step(&t, 40) == 1);      // reached target: release + emit
    assert(t.pending == 0);
    assert(takeover_step(&t, 20) == 1);      // released: tracks normally
}

static void test_takeover_catch_from_below(void) {
    takeover_t t = {0};
    assert(takeover_arm(&t, 10, 80) == 1);   // physical below the target -> catch
    assert(t.pending == 1 && t.from_above == 0);
    assert(takeover_step(&t, 50) == 0);      // still below: suppress
    assert(takeover_step(&t, 79) == 0);      // just below: suppress
    assert(takeover_step(&t, 80) == 1);      // reached target: release + emit
    assert(t.pending == 0);
}

static void test_takeover_catches_on_overshoot(void) {
    takeover_t t = {0};
    assert(takeover_arm(&t, 10, 60) == 1);   // below target
    assert(takeover_step(&t, 90) == 1);      // jumped past target in one read: still catches
    assert(t.pending == 0);
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
    test_fader_rescales_to_full_0_127();
    test_fader_suppresses_jitter_and_duplicates();
    test_fader_hysteresis_boundary_emits();
    test_fader_top_rail_bypasses_jitter_window();
    test_fader_bottom_reachable_and_reinit_reemits();
    test_takeover_no_catch_when_at_target();
    test_takeover_catch_from_above();
    test_takeover_catch_from_below();
    test_takeover_catches_on_overshoot();
    test_ladder_decode_matches_first_level_within_tolerance();
    printf("all control_logic tests passed\n");
    return 0;
}
