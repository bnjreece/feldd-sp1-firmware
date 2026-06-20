// midi1_codec.c — MIDI (status,d1,d2) -> 4-byte USB-MIDI 1.0 event packet.
#include "midi1_codec.h"

int midi1_event(uint8_t cable, uint8_t status, uint8_t d1, uint8_t d2, uint8_t out[4])
{
    uint8_t hi = (uint8_t)(status >> 4);   // channel-voice message type
    if (hi < 0x8 || hi > 0xE) {
        return -1;                         // not a channel-voice message
    }
    out[0] = (uint8_t)((cable << 4) | hi); // CIN == status high nibble for channel-voice
    out[1] = status;
    out[2] = d1;
    out[3] = (hi == 0xC || hi == 0xD) ? 0 : d2;   // program / channel-AT are 2-byte
    return 4;
}
