#include <assert.h>
#include <stdio.h>
#include "../src/clock_cfg.h"

static void test_roundtrip_every_field(void) {
    struct clock_cfg c = { .enable = 1, .tap_button = 6, .bpm_fader = 3, .default_bpm = 128 };
    uint8_t p[2];
    clock_cfg_pack(&c, p);
    struct clock_cfg r;
    clock_cfg_unpack(p, &r);
    assert(r.enable == 1 && r.tap_button == 6 && r.bpm_fader == 3 && r.default_bpm == 128);
}

static void test_enable_is_bit7(void) {
    struct clock_cfg on  = { .enable = 1, .tap_button = 0, .bpm_fader = 0, .default_bpm = 120 };
    struct clock_cfg off = { .enable = 0, .tap_button = 0, .bpm_fader = 0, .default_bpm = 120 };
    uint8_t p[2];
    clock_cfg_pack(&on, p);
    assert((p[0] & 0x80u) == 0x80u);
    clock_cfg_pack(&off, p);
    assert((p[0] & 0x80u) == 0u);
}

static void test_tap_button_bits_3_to_6(void) {
    struct clock_cfg c = { .enable = 0, .tap_button = 5, .bpm_fader = 0, .default_bpm = 120 };
    uint8_t p[2];
    clock_cfg_pack(&c, p);
    // tap_button 5 = 0b0101 lands in bits 3..6 -> 5 << 3 == 0x28
    assert(p[0] == 0x28);
    assert(((p[0] >> 3) & 0x0Fu) == 5);
}

static void test_bpm_fader_bits_0_to_2(void) {
    struct clock_cfg c = { .enable = 0, .tap_button = 0, .bpm_fader = 2, .default_bpm = 120 };
    uint8_t p[2];
    clock_cfg_pack(&c, p);
    // bpm_fader 2 in bits 0..2
    assert(p[0] == 0x02);
    assert((p[0] & 0x07u) == 2);
}

static void test_none_sentinels_roundtrip(void) {
    struct clock_cfg c = { .enable = 1, .tap_button = CLOCK_TAP_NONE,
                           .bpm_fader = CLOCK_FADER_NONE, .default_bpm = 90 };
    uint8_t p[2];
    clock_cfg_pack(&c, p);
    struct clock_cfg r;
    clock_cfg_unpack(p, &r);
    assert(r.tap_button == CLOCK_TAP_NONE);
    assert(r.bpm_fader == CLOCK_FADER_NONE);
    // enable set + both sentinels -> 0x80 | (0xF<<3) | 0x7 == 0xFF
    assert(p[0] == 0xFF);
}

static void test_default_bpm_is_byte1(void) {
    struct clock_cfg c = { .enable = 1, .tap_button = 0, .bpm_fader = 0, .default_bpm = 120 };
    uint8_t p[2];
    clock_cfg_pack(&c, p);
    assert(p[1] == 120);
}

static void test_fully_disabled(void) {
    // fully disabled: enable 0, no tap, no fader; bpm still carried in out[1]
    struct clock_cfg c = { .enable = 0, .tap_button = CLOCK_TAP_NONE,
                           .bpm_fader = CLOCK_FADER_NONE, .default_bpm = 40 };
    uint8_t p[2];
    clock_cfg_pack(&c, p);
    // 0x00 | (0xF<<3) | 0x7 == 0x7F, bit7 clear
    assert(p[0] == 0x7F);
    assert((p[0] & 0x80u) == 0u);
    assert(p[1] == 40);
    struct clock_cfg r;
    clock_cfg_unpack(p, &r);
    assert(r.enable == 0 && r.default_bpm == 40);
}

int main(void) {
    test_roundtrip_every_field();
    test_enable_is_bit7();
    test_tap_button_bits_3_to_6();
    test_bpm_fader_bits_0_to_2();
    test_none_sentinels_roundtrip();
    test_default_bpm_is_byte1();
    test_fully_disabled();
    printf("all clock_cfg tests passed\n");
    return 0;
}
