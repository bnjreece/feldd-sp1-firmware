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

/* Pull a channel-voice message out of a 4-byte USB-MIDI 1.0 event packet for the
 * MIDI-thru path (USB-in -> TRS-out). CIN (pkt[0] low nibble) 0x8/0x9/0xA/0xB/0xE
 * are 3-byte messages (note off/on, poly-aftertouch, CC, pitch-bend); 0xC/0xD are
 * 2-byte (program change, channel pressure). On a hit, copies the status + data
 * bytes into out[] and returns the message length (2 or 3). Everything else --
 * real-time (CIN 0xF, owned by the clock feature), SysEx (0x4-0x7), system-common
 * (0x2/0x3), reserved -- returns 0 and leaves out[] untouched. Cable nibble
 * (pkt[0] high nibble) ignored. Pure; host-testable. */
uint8_t usb_midi_extract_voice(const uint8_t pkt[4], uint8_t out[3]);
#endif
