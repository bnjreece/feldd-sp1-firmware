#ifndef BTN_TOGGLE_H
#define BTN_TOGGLE_H
#include <stdint.h>
#include "profile.h"   /* NUM_LAYERS, NUM_BUTTONS */

/* Pure per-LAYER, per-BUTTON latched-toggle store for BTN_CC_TOGGLE (e.g. the
 * OP-XY per-track mute CC9 / portamento CC29). Each track-layer keeps its OWN
 * on/off bit, so muting track 3 (layer 2) and hopping to track 5 (layer 4) do
 * not share a mute bit. INDEPENDENT per track: do NOT reset on a layer hop
 * (that would re-mute a track on return); reset every layer only on a profile
 * switch. Header-only so main.c and the host test link the identical indexing
 * (mirrors layer_takeover.h). 8*9 = 72 bytes. */
struct btn_toggle { uint8_t bit[NUM_LAYERS][NUM_BUTTONS]; };

static inline void btn_toggle_init(struct btn_toggle *t) {
    for (int L = 0; L < NUM_LAYERS; L++)
        for (int b = 0; b < NUM_BUTTONS; b++) t->bit[L][b] = 0;
}
/* Flip one (layer, button) latch and return its NEW value (0/1). */
static inline uint8_t btn_toggle_flip(struct btn_toggle *t, int layer, int idx) {
    t->bit[layer][idx] ^= 1u;
    return t->bit[layer][idx];
}
static inline uint8_t btn_toggle_get(const struct btn_toggle *t, int layer, int idx) {
    return t->bit[layer][idx];
}
/* Profile switch = full reset: clear EVERY layer's latch. */
static inline void btn_toggle_reset_all(struct btn_toggle *t) {
    btn_toggle_init(t);
}

#endif /* BTN_TOGGLE_H */
