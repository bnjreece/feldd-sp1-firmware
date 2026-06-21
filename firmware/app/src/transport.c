/* transport.c — map a BTN_TRANSPORT value + run state to a MIDI system
 * real-time status byte. See transport.h for the full contract. Pure. */
#include "transport.h"

/* MIDI system real-time transport status bytes. */
#define RT_START    0xFA
#define RT_CONTINUE 0xFB
#define RT_STOP     0xFC

uint8_t transport_rt(unsigned char value, int *playing)
{
    switch (value) {
    case 1:  *playing = 1; return RT_START;
    case 2:  *playing = 0; return RT_STOP;
    case 3:  *playing = 1; return RT_CONTINUE;
    case 0:  default:      /* play/stop toggle off the current run state */
        if (*playing) { *playing = 0; return RT_STOP; }
        *playing = 1; return RT_START;
    }
}
