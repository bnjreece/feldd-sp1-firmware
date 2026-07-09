#ifndef PANIC_H
#define PANIC_H
#include "mapping.h"   /* struct midi_msg */

/* Fill `out` (needs cap >= 48) with the MIDI panic set: for each channel 0..15,
 * CC 120 (all sound off), CC 121 (reset all controllers), CC 123 (all notes off),
 * all value 0, in that order. Returns the count written (48), or 0 if cap < 48. */
int panic_fill(struct midi_msg *out, int cap);
#endif
