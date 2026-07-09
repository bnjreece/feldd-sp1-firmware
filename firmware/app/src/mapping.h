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
/* Feature 1: read a BTN_CC_VALUE button's {sub_mode,on,off} for `layer` (DIRECT
 * 0..NUM_LAYERS-1, same indexing as profile_layer_chord). Returns 0 and fills the
 * out-params, or -1 for a NULL profile / out-of-range idx or layer. */
int profile_layer_ccval(const struct profile *p, int idx, int layer,
                        uint8_t *sub, uint8_t *on, uint8_t *off);
/* v7: a fader's role for `layer` (DIRECT 0..3). 0=cc, 1=chord_depth. */
uint8_t profile_layer_fader_role(const struct profile *p, int idx, int layer);

/* 0.22 Feature 1: resolve a profile's configurable PLAY shift target. Stored in
 * the reused v9 byte chord_flags[1]. Legacy 0 (every existing v9 profile) or an
 * out-of-range value (>= NUM_LAYERS) means "default", which is layer index 1
 * (UI L2, the historical behavior); explicit values 1..7 are honored. Pure. */
uint8_t profile_shift_target(const struct profile *p);

/* Home layer the PLAY shift flips to when you are ALREADY on the shift target
 * (so the momentary shift is never a no-op). Index 0 = UI layer L1. */
#define SHIFT_HOME 0

/* Feature 4: the LAYER a PLAY-shift build routes buttons/faders through. In
 * shift mode (play_mode 0) a held PLAY momentarily shifts to shift_target (the
 * profile's resolved PLAY shift target, see profile_shift_target); otherwise the
 * engaged gesture layer is used. Assignable mode (play_mode 1) never shifts, so
 * PLAY is a plain assignable button. Pure/host-tested. */
int effective_layer(int play_mode, int play_held, int gesture_layer, int shift_target);

/* fader idx 0..3, cc value 0..127 (already from fader_update); layer 0..3 */
void map_fader(const struct profile *p, int idx, int cc_val, int layer,
               midi_sink_fn sink, void *ctx);
/* button idx 0..8, pressed = 1/0; layer 0..3 */
void map_button(const struct profile *p, int idx, int pressed, int layer,
                midi_sink_fn sink, void *ctx);
#endif
