#include <assert.h>
#include <stdio.h>
#include "../src/gesture.h"

/* Host tests for the PLAY engaged-layer holder after Feature 4 retired the peek
 * FSM. gesture_t now carries ONLY the engaged layer: it is SET by main.c's
 * ••+FWD/RWD combo (gesture_set_layer(gesture_layer_step(...))) and READ by the
 * side-row LED + effective_layer(). gesture_step / gesture_disarm and the whole
 * peek/rocker plateau machinery are gone. */

/* --- 1. gesture_set_layer engages the layer (SET by main.c's ••+rocker combo).
 *        Clamps into range. --- */
static void test_set_layer_engages_and_clamps(void) {
    gesture_t g;
    gesture_init(&g);
    assert(gesture_layer(&g) == 0);

    gesture_set_layer(&g, 1);
    assert(gesture_layer(&g) == 1);

    gesture_set_layer(&g, 3);
    assert(gesture_layer(&g) == 3);

    /* mid-range layers 5 and 7 are reachable at the 8-layer ceiling. */
    gesture_set_layer(&g, 5);
    assert(gesture_layer(&g) == 5);
    gesture_set_layer(&g, 7);
    assert(gesture_layer(&g) == 7);

    gesture_set_layer(&g, 0);
    assert(gesture_layer(&g) == 0);

    /* over-range clamps to the top layer (GESTURE_LAYER_COUNT-1 = 7); negative
     * clamps to 0. main.c clamps first, but the setter guards independently. */
    gesture_set_layer(&g, 9);
    assert(gesture_layer(&g) == GESTURE_LAYER_COUNT - 1);
    assert(gesture_layer(&g) == 7);
    gesture_set_layer(&g, -2);
    assert(gesture_layer(&g) == 0);
}

/* gesture_layer_step: relative +1/-1 with wrap at GESTURE_LAYER_COUNT (••+rocker). */
static void test_layer_step_wraps_both_directions(void){
    assert(gesture_layer_step(0, +1) == 1);
    assert(gesture_layer_step(6, +1) == 7);
    assert(gesture_layer_step(GESTURE_LAYER_COUNT - 1, +1) == 0);   /* 7 -> 0 */
    assert(gesture_layer_step(0, -1) == GESTURE_LAYER_COUNT - 1);   /* 0 -> 7 */
    assert(gesture_layer_step(7, -1) == 6);
}

/* the gesture_shift() / gesture_latched() accessor (back-compat for main.c's
 * shift bank read) tracks the engaged layer's low bit so a 2-layer build still
 * reads a 0/1 shift flag */
static void test_shift_accessor_tracks_layer(void) {
    gesture_t g;
    gesture_init(&g);
    assert(gesture_shift(&g) == 0);
    gesture_set_layer(&g, 1);
    assert(gesture_shift(&g) == 1);                    /* layer 1 -> shift on */
    assert(gesture_latched(&g) == 1);
    gesture_set_layer(&g, 2);
    assert(gesture_shift(&g) == 0);                    /* layer 2 low bit = 0 */
    gesture_set_layer(&g, 3);
    assert(gesture_shift(&g) == 1);                    /* layer 3 low bit = 1 */
}

int main(void) {
    test_set_layer_engages_and_clamps();
    test_layer_step_wraps_both_directions();
    test_shift_accessor_tracks_layer();
    printf("all gesture tests passed\n");
    return 0;
}
