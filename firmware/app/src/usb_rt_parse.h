#ifndef USB_RT_PARSE_H
#define USB_RT_PARSE_H
#include <stdint.h>
/* Pull a MIDI system real-time byte out of a 4-byte USB-MIDI 1.0 event packet.
 * pkt[0] = (cable<<4) | CIN. Single-byte messages (all system real-time) use
 * CIN 0xF with the status byte in pkt[1]. Returns pkt[1] when (pkt[0] & 0x0F)
 * == 0xF AND pkt[1] is a transport/clock byte we forward: Clock 0xF8, Start
 * 0xFA, Continue 0xFB, Stop 0xFC. Everything else (wrong CIN, non-status,
 * non-transport real-time like active-sensing 0xFE, system-common) returns 0.
 * Cable number (high nibble of pkt[0]) is ignored -- any cable matches.
 * Pure; host-testable. */
uint8_t usb_midi_extract_rt(const uint8_t pkt[4]);
#endif
