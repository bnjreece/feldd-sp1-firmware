#include "mapping.h"
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
void map_fader(const struct profile *p, int idx, int cc_val, int shift,
               midi_sink_fn sink, void *ctx) {
    if (idx < 0 || idx >= NUM_FADERS || cc_val < 0) return;
    uint8_t cc = shift ? p->shift.fader_cc[idx] : p->fader[idx].cc;
    uint8_t val = scale(cc_val, &p->fader[idx]);
    /* v2: each fader rides its own channel (p->fader_channel[idx]), so 4 faders
     * can drive 4 different tracks at once. */
    struct midi_msg m = { .status = 0xB0 | (p->fader_channel[idx] & 0x0F), .d1 = cc, .d2 = val, .len = 3 };
    sink(&m, ctx);
}
void map_button(const struct profile *p, int idx, int pressed, int shift,
                midi_sink_fn sink, void *ctx) {
    if (idx < 0 || idx >= NUM_BUTTONS) return;
    const struct button_map *b = &p->button[idx];
    uint8_t ch = p->button_channel[idx] & 0x0F;   /* v2: per-button channel */
    uint8_t val = shift ? p->shift.button_value[idx] : b->value;
    struct midi_msg m = { .len = 3 };
    switch (b->type) {
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
