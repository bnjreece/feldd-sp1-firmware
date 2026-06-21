#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../src/layer_takeover.h"

/* The soft-takeover bank memory must be per-fader AND per-LAYER (4 layers), so a
 * 4-layer profile remembers each layer's last CC independently and only the
 * active layer's slot is read/written on a fader move. This pins the indexing
 * that main.c's g_bank_last_cc / g_bank_valid widen to [NUM_FADERS][NUM_LAYERS]. */

static void test_four_layer_banks_independent(void) {
    struct layer_takeover lt;
    layer_takeover_init(&lt);
    /* nothing valid initially, every (fader,layer) */
    for (int f = 0; f < NUM_FADERS; f++)
        for (int L = 0; L < NUM_LAYERS; L++)
            assert(layer_takeover_valid(&lt, f, L) == 0);
    /* record a distinct last CC on every layer of fader 1 */
    for (int L = 0; L < NUM_LAYERS; L++)
        layer_takeover_record(&lt, 1, L, (unsigned char)(10 + L));
    for (int L = 0; L < NUM_LAYERS; L++) {
        assert(layer_takeover_valid(&lt, 1, L) == 1);
        assert(layer_takeover_last(&lt, 1, L) == (unsigned char)(10 + L));
    }
    /* a different fader is untouched */
    assert(layer_takeover_valid(&lt, 0, 3) == 0);
}

static void test_clear_drops_all_layers(void) {
    struct layer_takeover lt;
    layer_takeover_init(&lt);
    for (int L = 0; L < NUM_LAYERS; L++)
        layer_takeover_record(&lt, 2, L, (unsigned char)(L + 1));
    layer_takeover_clear_fader(&lt, 2);              /* profile/mode switch path */
    for (int L = 0; L < NUM_LAYERS; L++)
        assert(layer_takeover_valid(&lt, 2, L) == 0);
}

int main(void) {
    test_four_layer_banks_independent();
    test_clear_drops_all_layers();
    printf("all layer_bank tests passed\n");
    return 0;
}
