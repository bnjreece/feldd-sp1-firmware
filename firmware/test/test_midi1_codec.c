#include <assert.h>
#include <stdio.h>
#include "../src/midi1_codec.h"

static void test_channel_voice_packing(void) {
    uint8_t p[4];
    // CC ch1 (0xB0) cc7=100 -> CIN 0xB, cable 0
    assert(midi1_event(0, 0xB0, 7, 100, p) == 4);
    assert(p[0] == 0x0B && p[1] == 0xB0 && p[2] == 7 && p[3] == 100);
    // Note-on ch3 (0x92) note60 vel80 -> CIN 0x9
    assert(midi1_event(0, 0x92, 60, 80, p) == 4);
    assert(p[0] == 0x09 && p[1] == 0x92 && p[2] == 60 && p[3] == 80);
    // Note-off (0x80) -> CIN 0x8
    assert(midi1_event(0, 0x80, 60, 0, p) == 4);
    assert(p[0] == 0x08);
    // pitch bend (0xE0) -> CIN 0xE, 3-byte
    assert(midi1_event(0, 0xE0, 0, 64, p) == 4);
    assert(p[0] == 0x0E && p[2] == 0 && p[3] == 64);
}

static void test_two_byte_messages_zero_d2(void) {
    uint8_t p[4];
    // program change (0xC0) -> CIN 0xC, d2 forced 0 even if passed
    assert(midi1_event(0, 0xC0, 5, 99, p) == 4);
    assert(p[0] == 0x0C && p[1] == 0xC0 && p[2] == 5 && p[3] == 0);
    // channel aftertouch (0xD0) -> CIN 0xD, d2 forced 0
    assert(midi1_event(0, 0xD0, 40, 99, p) == 4);
    assert(p[0] == 0x0D && p[2] == 40 && p[3] == 0);
}

static void test_cable_nibble_and_rejects(void) {
    uint8_t p[4];
    // cable goes in the high nibble of byte 0
    assert(midi1_event(1, 0xB0, 1, 2, p) == 4);
    assert(p[0] == 0x1B);
    // system real-time (0xF8 clock) -> reject (hi 0xF > 0xE)
    assert(midi1_event(0, 0xF8, 0, 0, p) == -1);
    // system common (0xF0 sysex start) -> reject
    assert(midi1_event(0, 0xF0, 0, 0, p) == -1);
    // a data byte mistaken as status (0x40, hi 0x4 < 0x8) -> reject
    assert(midi1_event(0, 0x40, 0, 0, p) == -1);
}

int main(void) {
    test_channel_voice_packing();
    test_two_byte_messages_zero_d2();
    test_cable_nibble_and_rejects();
    printf("all midi1_codec tests passed\n");
    return 0;
}
