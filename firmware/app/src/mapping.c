#include <stddef.h>   /* NULL (profile_layer_chord) */
#include "mapping.h"
#include "chord6.h"
static uint8_t scale(int cc_val, const struct fader_map *f) {
    int v = cc_val;                          /* 0..127 from fader_update */
    if (f->invert) v = 127 - v;
    if (f->curve == CURVE_LOG)  v = (v * v) / 127;                          /* slow start, fast end */
    else if (f->curve == CURVE_EXP) v = 127 - ((127 - v) * (127 - v)) / 127; /* fast start, slow end */
    /* any unrecognized curve value falls through to linear */
    int span = (int)f->max - (int)f->min;
    v = (int)f->min + (v * span) / 127;
    if (v < 0) v = 0; if (v > 127) v = 127;
    return (uint8_t)v;
}
/* v5: per-layer bank select (0=inline L1, 1=shift L2, 2=layer[0] L3, 3=layer[1]
 * L4). map_fader/map_button + main.c's Keyboard path route through these so the
 * gesture_layer() (0..3) drives one consistent select. default = layer 0. */
uint8_t profile_layer_fader_cc(const struct profile *p, int idx, int layer) {
    switch (layer) {
    case 1:  return p->shift.fader_cc[idx];
    case 2:  return p->layer[0].fader_cc[idx];
    case 3:  return p->layer[1].fader_cc[idx];
    default: return p->fader[idx].cc;          /* layer 0 = inline */
    }
}
uint8_t profile_layer_button_value(const struct profile *p, int idx, int layer) {
    switch (layer) {
    case 1:  return p->shift.button_value[idx];
    case 2:  return p->layer[0].button_value[idx];
    case 3:  return p->layer[1].button_value[idx];
    default: return p->button[idx].value;
    }
}
uint8_t profile_layer_button_key(const struct profile *p, int idx, int layer) {
    switch (layer) {
    case 1:  return p->button_key_shift[idx];
    case 2:  return p->layer[0].button_key[idx];
    case 3:  return p->layer[1].button_key[idx];
    default: return p->button_key[idx];
    }
}
uint8_t profile_layer_button_mod(const struct profile *p, int idx, int layer) {
    switch (layer) {
    case 1:  return p->button_mod_shift[idx];
    case 2:  return p->layer[0].button_mod[idx];
    case 3:  return p->layer[1].button_mod[idx];
    default: return p->button_mod[idx];
    }
}
/* v6: the fader scale fields + per-control channels + button type are now read
 * from the ACTIVE layer's own bank. Layer 0 = L1 inline; layers 1..3 = ext[L-1]
 * (L2/L3/L4). The ext index is (layer-1); any out-of-range layer falls to L1. */
uint8_t profile_layer_fader_min(const struct profile *p, int idx, int layer) {
    if (layer >= 1 && layer <= 3) return p->ext[layer - 1].fader_min[idx];
    return p->fader[idx].min;
}
uint8_t profile_layer_fader_max(const struct profile *p, int idx, int layer) {
    if (layer >= 1 && layer <= 3) return p->ext[layer - 1].fader_max[idx];
    return p->fader[idx].max;
}
uint8_t profile_layer_fader_curve(const struct profile *p, int idx, int layer) {
    if (layer >= 1 && layer <= 3) return p->ext[layer - 1].fader_curve[idx];
    return p->fader[idx].curve;
}
uint8_t profile_layer_fader_invert(const struct profile *p, int idx, int layer) {
    if (layer >= 1 && layer <= 3) return p->ext[layer - 1].fader_invert[idx];
    return p->fader[idx].invert;
}
uint8_t profile_layer_fader_channel(const struct profile *p, int idx, int layer) {
    if (layer >= 1 && layer <= 3) return p->ext[layer - 1].fader_channel[idx];
    return p->fader_channel[idx];
}
uint8_t profile_layer_button_type(const struct profile *p, int idx, int layer) {
    if (layer >= 1 && layer <= 3) return p->ext[layer - 1].button_type[idx];
    return p->button[idx].type;
}
uint8_t profile_layer_button_channel(const struct profile *p, int idx, int layer) {
    if (layer >= 1 && layer <= 3) return p->ext[layer - 1].button_channel[idx];
    return p->button_channel[idx];
}
/* v8: unpack a button's chord for `layer` (DIRECT 0..3) into *out. NULL if the slot
 * is empty (chord6_is_empty) or the indices are out of range. The unpacked def is
 * what chord_engine's chord_resolve consumes (MAX_CHORD=8 ranges/depth unchanged). */
const struct chord_def *profile_layer_chord(const struct profile *p,
                                            int idx, int layer,
                                            struct chord_def *out)
{
    if (out == 0) return 0;
    if (layer < 0 || layer >= NUM_LAYERS) return 0;
    if (idx   < 0 || idx   >= NUM_BUTTONS) return 0;
    const struct chord6 *c = &p->chord6[layer][idx];
    if (chord6_is_empty(c)) return 0;
    chord6_unpack(c, out);
    return out;
}
/* v7: a fader's role for `layer` (DIRECT 0..3). 0=cc, 1=chord_depth. */
uint8_t profile_layer_fader_role(const struct profile *p, int idx, int layer) {
    int L = (layer >= 0 && layer < NUM_LAYERS) ? layer : 0;
    int i = (idx   >= 0 && idx   < NUM_FADERS) ? idx   : 0;
    return p->fader_role[L][i];
}
void map_fader(const struct profile *p, int idx, int cc_val, int layer,
               midi_sink_fn sink, void *ctx) {
    if (idx < 0 || idx >= NUM_FADERS || cc_val < 0) return;
    uint8_t cc = profile_layer_fader_cc(p, idx, layer);
    /* v6: min/max/curve/invert come from the ACTIVE layer's bank, not L1. */
    struct fader_map f = {
        .cc     = cc,
        .min    = profile_layer_fader_min(p, idx, layer),
        .max    = profile_layer_fader_max(p, idx, layer),
        .curve  = profile_layer_fader_curve(p, idx, layer),
        .invert = profile_layer_fader_invert(p, idx, layer),
    };
    uint8_t val = scale(cc_val, &f);
    /* v2/v6: each fader rides its own per-layer channel, so 4 faders can drive 4
     * different tracks at once, and each layer is an independent mixer. */
    uint8_t ch = profile_layer_fader_channel(p, idx, layer) & 0x0F;
    struct midi_msg m = { .status = 0xB0 | ch, .d1 = cc, .d2 = val, .len = 3 };
    sink(&m, ctx);
}
void map_button(const struct profile *p, int idx, int pressed, int layer,
                midi_sink_fn sink, void *ctx) {
    if (idx < 0 || idx >= NUM_BUTTONS) return;
    /* v6: button TYPE + channel come from the ACTIVE layer's bank, not L1. */
    uint8_t type = profile_layer_button_type(p, idx, layer);
    uint8_t ch = profile_layer_button_channel(p, idx, layer) & 0x0F;
    uint8_t val = profile_layer_button_value(p, idx, layer);
    struct midi_msg m = { .len = 3 };
    switch (type) {
    case BTN_NOTE:
        m.status = (pressed ? 0x90 : 0x80) | ch; m.d1 = val; m.d2 = pressed ? 127 : 0;
        sink(&m, ctx); break;
    case BTN_CC_MOMENTARY:
        m.status = 0xB0 | ch; m.d1 = val; m.d2 = pressed ? 127 : 0; sink(&m, ctx); break;
    /* Latching toggle / transport / profile-switch are STATEFUL: the firmware
     * control loop (main.c, later milestone) owns their on/off + transport state
     * and emits the MIDI itself. The pure engine intentionally emits nothing. */
    case BTN_CC_TOGGLE: case BTN_TRANSPORT: case BTN_PROFILE_SWITCH:
    case BTN_NONE: default: break;
    }
}
