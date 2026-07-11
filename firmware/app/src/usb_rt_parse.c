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
