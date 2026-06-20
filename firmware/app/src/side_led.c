#include "side_led.h"
#include "mode.h"

/* Pure SIDE-row render. See side_led.h for the full contract. Mirrors
 * dial_track_pattern: one pure function, time in scan ticks, writes out[0..3].
 *
 * Bit order is mode_led_pattern's: bit i (pat >> i) & 1 == side LED i, so MIDI
 * (MODE_LED1 | MODE_LED4 = bits 0,3) -> {1,0,0,1} and KEYBOARD (MODE_LED2 |
 * MODE_LED3 = bits 1,2) -> {0,1,1,0}. The flash render is derived from
 * mode_led_pattern so the side row and the mode table can never desync. */
void side_led_pattern(unsigned char layer, unsigned int flash_ticks,
                      unsigned char flash_mode, int flash_blink,
                      unsigned char tick, unsigned char out[4])
{
    int i;

    if (flash_ticks > 0) {
        /* MODE-FLASH (transient, highest priority): show the captured mode's
         * pattern. PULSE (flash_blink == 0, the calm default) holds the pattern
         * solid for the whole window; BLINK toggles it on/off on the same
         * ~12-tick half-period the dial 5-8 blink and the charge cue use. */
        uint8_t pat = mode_led_pattern(flash_mode);
        int lit = flash_blink ? ((tick / 12) & 1) : 1;
        for (i = 0; i < 4; i++) {
            out[i] = (lit && ((pat >> i) & 1u)) ? 1 : 0;
        }
        return;
    }

    /* LAYER rest (permanent): exactly ONE side LED lit at the layer position,
     * natural layer-to-pin ordering (layer 0 -> SP1_PLAY_LED1). NEVER a bar. */
    for (i = 0; i < 4; i++) {
        out[i] = (i == layer) ? 1 : 0;
    }
}
