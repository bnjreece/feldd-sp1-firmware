#include <assert.h>
#include <stdio.h>
#include "../src/btn_toggle.h"

/* Latching a mute on track-layer 3 (index 2) must NOT flip track-layer 5
 * (index 4): independent per-track state is the whole point of 8 layers. */
static void test_layer3_mute_does_not_flip_layer5(void) {
    struct btn_toggle t;
    btn_toggle_init(&t);
    assert(btn_toggle_flip(&t, 2, 1) == 1);   /* mute T1 on L3 */
    assert(btn_toggle_get(&t, 2, 1) == 1);
    assert(btn_toggle_get(&t, 4, 1) == 0);    /* L5 T1 untouched */
    assert(btn_toggle_flip(&t, 4, 1) == 1);   /* mute T1 on L5 */
    assert(btn_toggle_get(&t, 2, 1) == 1);    /* L3 still muted */
    assert(btn_toggle_get(&t, 4, 1) == 1);
}

/* Every layer keeps its own bit for the same button idx across all 8 layers. */
static void test_all_layers_independent(void) {
    struct btn_toggle t;
    btn_toggle_init(&t);
    for (int L = 0; L < NUM_LAYERS; L++) assert(btn_toggle_flip(&t, L, 1) == 1);
    for (int L = 0; L < NUM_LAYERS; L++) assert(btn_toggle_get(&t, L, 1) == 1);
    assert(btn_toggle_flip(&t, 0, 1) == 0);   /* L1 back off */
    for (int L = 1; L < NUM_LAYERS; L++) assert(btn_toggle_get(&t, L, 1) == 1);
}

/* A profile switch is a full state reset: clear EVERY layer's latch. */
static void test_profile_switch_clears_all_layers(void) {
    struct btn_toggle t;
    btn_toggle_init(&t);
    for (int L = 0; L < NUM_LAYERS; L++)
        for (int b = 0; b < NUM_BUTTONS; b++) t.bit[L][b] = 1;
    btn_toggle_reset_all(&t);
    for (int L = 0; L < NUM_LAYERS; L++)
        for (int b = 0; b < NUM_BUTTONS; b++) assert(btn_toggle_get(&t, L, b) == 0);
}

int main(void) {
    test_layer3_mute_does_not_flip_layer5();
    test_all_layers_independent();
    test_profile_switch_clears_all_layers();
    printf("all btn_toggle tests passed\n");
    return 0;
}
