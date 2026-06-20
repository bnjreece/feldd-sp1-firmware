#ifndef MIDI_OUT_H
#define MIDI_OUT_H
#include "mapping.h"
/* TRS UART MIDI output path (USB-MIDI sink is added in M4).
 * midi_out_send matches the mapping engine's midi_sink_fn signature, so it can
 * be passed straight to map_fader()/map_button() as the sink. */
int  midi_out_init(void);
void midi_out_send(const struct midi_msg *m, void *ctx);   /* matches midi_sink_fn */
#endif
