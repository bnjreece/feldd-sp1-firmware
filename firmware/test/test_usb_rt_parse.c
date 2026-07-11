#include <assert.h>
#include <stdio.h>
#include "../src/usb_rt_parse.h"

static void test_realtime_transport_and_clock(void) {
    // Clock 0xF8 on cable 0, CIN 0xF
    uint8_t clk[4]  = {0x0F, 0xF8, 0, 0};
    assert(usb_midi_extract_rt(clk) == 0xF8);
    // Start 0xFA
    uint8_t start[4] = {0x0F, 0xFA, 0, 0};
    assert(usb_midi_extract_rt(start) == 0xFA);
    // Continue 0xFB
    uint8_t cont[4]  = {0x0F, 0xFB, 0, 0};
    assert(usb_midi_extract_rt(cont) == 0xFB);
    // Stop 0xFC
    uint8_t stop[4]  = {0x0F, 0xFC, 0, 0};
    assert(usb_midi_extract_rt(stop) == 0xFC);
}

static void test_cable_nibble_ignored(void) {
    // Clock on cable 1 (pkt[0] = 0x1F): cable is ignored, still extracts.
    uint8_t clk_c1[4] = {0x1F, 0xF8, 0, 0};
    assert(usb_midi_extract_rt(clk_c1) == 0xF8);
}

static void test_non_realtime_rejected(void) {
    // Note-on (CIN 0x9) -> not a single-byte real-time packet.
    uint8_t noteon[4] = {0x09, 0x90, 60, 100};
    assert(usb_midi_extract_rt(noteon) == 0);
    // Tune request 0xF6: single-byte CIN 0xF but not a transport/clock byte.
    uint8_t tune[4] = {0x0F, 0xF6, 0, 0};
    assert(usb_midi_extract_rt(tune) == 0);
    // Active sensing 0xFE: real-time but not transport/clock -> drop.
    uint8_t sense[4] = {0x0F, 0xFE, 0, 0};
    assert(usb_midi_extract_rt(sense) == 0);
}

int main(void) {
    test_realtime_transport_and_clock();
    test_cable_nibble_ignored();
    test_non_realtime_rejected();
    printf("all usb_rt_parse tests passed\n");
    return 0;
}
