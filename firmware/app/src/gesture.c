#include "gesture.h"

void gesture_init(gesture_t *g) {
    g->layer = 0;   /* Feature 4: gesture_t is now just the engaged-layer holder. */
}

int gesture_layer(const gesture_t *g) {
    return g->layer;
}

void gesture_set_layer(gesture_t *g, int layer) {
    /* Clamp into 0..GESTURE_LAYER_COUNT-1. main.c's layer count-dial already
     * clamps an over-tap to the top layer (index 7 at 8 layers) before calling,
     * but guard here too so a stray value can never index past the side LED row. */
    if (layer < 0) {
        layer = 0;
    } else if (layer > GESTURE_LAYER_COUNT - 1) {
        layer = GESTURE_LAYER_COUNT - 1;
    }
    g->layer = (uint8_t)layer;
}

int gesture_shift(const gesture_t *g) {
    return g->layer & 1;
}

int gesture_latched(const gesture_t *g) {
    return gesture_shift(g);
}

int gesture_layer_step(int layer, int delta) {
    int n = GESTURE_LAYER_COUNT;
    return ((layer + delta) % n + n) % n;
}
