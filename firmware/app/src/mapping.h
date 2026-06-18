#ifndef MAPPING_H
#define MAPPING_H
#include <stdint.h>
#include "profile.h"
struct midi_msg { uint8_t status, d1, d2; uint8_t len; };  /* len 2 or 3 */
typedef void (*midi_sink_fn)(const struct midi_msg *m, void *ctx);
/* fader idx 0..3, cc value 0..127 (already from fader_update); shift = •• held */
void map_fader(const struct profile *p, int idx, int cc_val, int shift,
               midi_sink_fn sink, void *ctx);
/* button idx 0..8, pressed = 1/0; shift = •• held */
void map_button(const struct profile *p, int idx, int pressed, int shift,
                midi_sink_fn sink, void *ctx);
#endif
