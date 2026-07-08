#ifndef MAPPING_H
#define MAPPING_H
#include <stdint.h>
#include "profile.h"
struct midi_msg { uint8_t status, d1, d2; uint8_t len; };  /* len 2 or 3 */
typedef void (*midi_sink_fn)(const struct midi_msg *m, void *ctx);

/* v9: select the active per-layer bank by LAYER index (0..NUM_LAYERS-1).
 * 0 = inline L1, 1 = shift.* (L2), 2..NUM_LAYERS-1 = layer[layer-2] (L3..L8).
 * Callers pass gesture_layer() (always 0..NUM_LAYERS-1). MIDI reads
 * fader_cc + button_value; Keyboard reads button_key + button_mod. */
uint8_t profile_layer_fader_cc(const struct profile *p, int idx, int layer);
uint8_t profile_layer_button_value(const struct profile *p, int idx, int layer);
uint8_t profile_layer_button_key(const struct profile *p, int idx, int layer);
uint8_t profile_layer_button_mod(const struct profile *p, int idx, int layer);

/* v9: the fields that, in v5, were SHARED from L1 are now read from the ACTIVE
 * layer's own bank. Layer 0 = L1 inline (fader_map[idx], button[idx].type,
 * fader_channel[idx], button_channel[idx]); layers 1..NUM_LAYERS-1 = ext[layer-1]
 * (L2..L8). Out-of-range layer falls to L1 (defensive, like the v5 accessors). */
uint8_t profile_layer_fader_min(const struct profile *p, int idx, int layer);
uint8_t profile_layer_fader_max(const struct profile *p, int idx, int layer);
uint8_t profile_layer_fader_curve(const struct profile *p, int idx, int layer);
uint8_t profile_layer_fader_invert(const struct profile *p, int idx, int layer);
uint8_t profile_layer_fader_channel(const struct profile *p, int idx, int layer);
uint8_t profile_layer_button_type(const struct profile *p, int idx, int layer);
uint8_t profile_layer_button_channel(const struct profile *p, int idx, int layer);

/* v8: unpack a button's chord for `layer` (DIRECT 0..3) into *out. Returns out (a
 * configured chord) or NULL (empty slot / out-of-range). chord6 -> struct chord_def. */
const struct chord_def *profile_layer_chord(const struct profile *p, int idx, int layer,
                                            struct chord_def *out);
/* v7: a fader's role for `layer` (DIRECT 0..3). 0=cc, 1=chord_depth. */
uint8_t profile_layer_fader_role(const struct profile *p, int idx, int layer);

/* fader idx 0..3, cc value 0..127 (already from fader_update); layer 0..3 */
void map_fader(const struct profile *p, int idx, int cc_val, int layer,
               midi_sink_fn sink, void *ctx);
/* button idx 0..8, pressed = 1/0; layer 0..3 */
void map_button(const struct profile *p, int idx, int pressed, int layer,
                midi_sink_fn sink, void *ctx);
#endif
