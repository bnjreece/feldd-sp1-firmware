#ifndef MIDI1_CODEC_H
#define MIDI1_CODEC_H
#include <stdint.h>
/* Pack a channel-voice MIDI message (status,d1,d2) into a 4-byte USB-MIDI 1.0
 * event packet: out[0] = (cable<<4) | CIN, out[1..3] = the MIDI bytes. The Code
 * Index Number (CIN) for a channel-voice message equals the status high nibble
 * (note-off 0x8 .. pitch-bend 0xE). Program-change (0xC) and channel-aftertouch
 * (0xD) are 2-byte, so out[3] is forced to 0 for them. Returns 4 on success, or
 * -1 if status is not a channel-voice message. Pure; host-testable. */
int midi1_event(uint8_t cable, uint8_t status, uint8_t d1, uint8_t d2, uint8_t out[4]);
#endif
