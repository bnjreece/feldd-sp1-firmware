// usb_rt_parse.c -- extract a forwarded MIDI real-time byte from a USB-MIDI 1.0 packet.
#include "usb_rt_parse.h"

uint8_t usb_midi_extract_rt(const uint8_t pkt[4])
{
    if ((pkt[0] & 0x0F) != 0x0F) {   // CIN 0xF = single-byte message; ignore cable nibble
        return 0;
    }
    switch (pkt[1]) {                // only forward clock + transport
    case 0xF8:                       // clock
    case 0xFA:                       // start
    case 0xFB:                       // continue
    case 0xFC:                       // stop
        return pkt[1];
    default:                         // tune-request, active-sensing, reset, data... -> drop
        return 0;
    }
}

uint8_t usb_midi_extract_voice(const uint8_t pkt[4], uint8_t out[3])
{
    uint8_t cin = pkt[0] & 0x0F;     // CIN; cable nibble ignored
    uint8_t len;
    switch (cin) {
    case 0x8:                        // note off
    case 0x9:                        // note on
    case 0xA:                        // poly aftertouch
    case 0xB:                        // control change
    case 0xE:                        // pitch bend
        len = 3;
        break;
    case 0xC:                        // program change
    case 0xD:                        // channel pressure
        len = 2;
        break;
    default:                         // real-time (0xF, clock owns it), SysEx
        return 0;                    // (0x4-0x7), system-common (0x2/0x3), reserved
    }

    // Reject malformed packets so a buggy/hostile host can't push a raw status
    // byte (System Reset 0xFF, SysEx-start 0xF0) or non-7-bit data onto the
    // downstream TRS synth: the status high nibble MUST equal the CIN (which also
    // proves pkt[1] is a status byte), and the data bytes MUST be 7-bit.
    if ((pkt[1] >> 4) != cin) {
        return 0;
    }
    if (pkt[2] & 0x80u) {
        return 0;
    }
    if (len == 3 && (pkt[3] & 0x80u)) {
        return 0;
    }

    out[0] = pkt[1];
    out[1] = pkt[2];
    if (len == 3) {
        out[2] = pkt[3];
    }
    return len;
}
