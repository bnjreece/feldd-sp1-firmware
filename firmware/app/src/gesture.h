#ifndef GESTURE_H
#define GESTURE_H
#include <stdint.h>

/* Pure, host-testable PLAY engaged-layer holder for the SP-1.
 *
 * Feature 4 (feldd 0.21) retired the PLAY peek/mode gesture FSM and both count-
 * dials. gesture_t now carries ONLY the engaged layer index. The layer is SET by
 * main.c's dot-dot (••) + FWD/RWD combo (gesture_set_layer(gesture_layer_step(...)))
 * and READ by the side-row layer LED + effective_layer() (the PLAY momentary-shift
 * decision). PLAY itself is dispatched per-edge in main.c (shift-by-default /
 * assignable), and •• + T4 flips MIDI<->Keyboard via mode.c's combo engine. There
 * is no longer a press-edge / hold-plateau / rocker-voids-peek machine here. */

/* Number of layers (`layer` is 0..GESTURE_LAYER_COUNT-1). v9 raised this 4 -> 8 so
 * one profile drives all 8 OP-XY tracks (layers = tracks). Layers 0..7 map to the
 * side row's SOLID 1-4 / BLINK 5-8 dot. The layer is SET by main.c's ••+FWD/RWD
 * combo (gesture_set_layer, wrapping via gesture_layer_step). */
#define GESTURE_LAYER_COUNT 8

typedef struct {
    uint8_t  layer;        /* engaged layer index 0..GESTURE_LAYER_COUNT-1 */
} gesture_t;

void gesture_init(gesture_t *g);

/* Current engaged layer (0..GESTURE_LAYER_COUNT-1). */
int gesture_layer(const gesture_t *g);

/* Set the engaged layer (0..GESTURE_LAYER_COUNT-1). Called by main.c's ••+FWD/RWD
 * combo commit; out-of-range values are clamped to the top layer (index 7 at 8
 * layers) so a stray value can never index past the side LED row. Side-row layer
 * LED snaps to the new value on the next render tick. */
void gesture_set_layer(gesture_t *g, int layer);

/* Relative layer step with wrap at GESTURE_LAYER_COUNT. delta +1/-1 (the ••+FWD/
 * RWD combo layer nav): from layer 7 a +1 wraps to 0, from layer 0 a -1 wraps to
 * 7. Pure/host-tested; main.c feeds the result to gesture_set_layer. */
int gesture_layer_step(int layer, int delta);

/* Back-compat shift accessor: the layer's low bit (0/1). Feed as the `shift` arg
 * into map_fader/map_button for a 2-layer build (layer 1 = shift bank). Kept so
 * main.c's existing shift-bank read keeps working through the layer rename.
 * gesture_latched() is an alias for the same value. */
int gesture_shift(const gesture_t *g);
int gesture_latched(const gesture_t *g);

#endif /* GESTURE_H */
