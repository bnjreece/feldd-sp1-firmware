#include "mode.h"

struct mode_decision mode_decide(int modifier_hold_ticks, int idx, int pressed)
{
    struct mode_decision d = { 0, MODE_MIDI, 0 };

    if (modifier_hold_ticks < MODE_FLIP_ARM_SCANS) {
        return d;                 /* held too briefly (or not held) -> transport */
    }
    if (idx != MODE_BTN_FWD && idx != MODE_BTN_RWD) {
        return d;                 /* some other button under the modifier -> passthrough */
    }

    /* modifier held + a FWD/RWD edge: this is the mode gesture. Consume BOTH the
     * press and the release so the selector button never leaks a keystroke / MIDI
     * note. Only the PRESS actually sets the mode. */
    d.consumed = 1;
    if (pressed) {
        d.set   = 1;
        d.which = (idx == MODE_BTN_FWD) ? MODE_KEYBOARD : MODE_MIDI;
    }
    return d;
}

uint8_t mode_led_pattern(int mode)
{
    switch (mode) {
    case MODE_MIDI:     return MODE_LED1 | MODE_LED4;
    case MODE_KEYBOARD: return MODE_LED2 | MODE_LED3;
    case 2:             return MODE_LED1 | MODE_LED2;   /* reserved personality */
    case 3:             return MODE_LED3 | MODE_LED4;   /* reserved personality */
    default:            return MODE_LED1 | MODE_LED4;   /* clamp -> MIDI */
    }
}
