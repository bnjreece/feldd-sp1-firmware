#ifndef LAYER_TAKEOVER_H
#define LAYER_TAKEOVER_H
#include <stdint.h>
#include "profile.h"   /* NUM_FADERS, NUM_LAYERS */

/* Pure per-fader, per-LAYER soft-takeover ("pickup") memory. v4 tracked 2 banks
 * (base/shift) as [NUM_FADERS][2]; v5 tracks all NUM_LAYERS as [NUM_FADERS][NUM_LAYERS].
 * Each (fader, layer) remembers the last CC that layer emitted + a valid flag that
 * gates the first-ever emit (seed). main.c uses this to hold a layer's previous
 * value on a layer switch until the physical fader CROSSES it. Header-only so the
 * exact indexing links into both firmware (main.c) and the host test. */
struct layer_takeover {
    uint8_t last_cc[NUM_FADERS][NUM_LAYERS];
    uint8_t valid[NUM_FADERS][NUM_LAYERS];
};

static inline void layer_takeover_init(struct layer_takeover *t) {
    for (int f = 0; f < NUM_FADERS; f++)
        for (int L = 0; L < NUM_LAYERS; L++) { t->last_cc[f][L] = 0; t->valid[f][L] = 0; }
}
static inline int layer_takeover_valid(const struct layer_takeover *t, int f, int L) {
    return t->valid[f][L];
}
static inline uint8_t layer_takeover_last(const struct layer_takeover *t, int f, int L) {
    return t->last_cc[f][L];
}
static inline void layer_takeover_record(struct layer_takeover *t, int f, int L, uint8_t cc) {
    t->last_cc[f][L] = cc; t->valid[f][L] = 1;
}
static inline void layer_takeover_clear_fader(struct layer_takeover *t, int f) {
    for (int L = 0; L < NUM_LAYERS; L++) t->valid[f][L] = 0;
}

#endif /* LAYER_TAKEOVER_H */
