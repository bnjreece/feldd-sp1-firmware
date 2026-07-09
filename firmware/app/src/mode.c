#include "mode.h"

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

/* ---- Feature 4: dot-dot Fn-modifier combo engine ---- */
void combo_latch_init(combo_latch_t *l){ l->latched = 0; }

static uint8_t combo_action_for(int idx){
    switch (idx){
    case COMBO_BTN_T1:    return COMBO_BATTERY;
    case COMBO_BTN_T2:    return COMBO_BRIGHTNESS;
    case COMBO_BTN_T3:    return COMBO_PANIC;
    case COMBO_BTN_T4:    return COMBO_MODE_TOGGLE;
    case COMBO_BTN_VOLUP: return COMBO_PROFILE_NEXT;
    case COMBO_BTN_VOLDN: return COMBO_PROFILE_PREV;
    case COMBO_BTN_FWD:   return COMBO_LAYER_NEXT;
    case COMBO_BTN_RWD:   return COMBO_LAYER_PREV;
    default:              return COMBO_NONE;
    }
}

uint8_t mode_toggle(uint8_t cur){ return (cur == MODE_MIDI) ? MODE_KEYBOARD : MODE_MIDI; }

struct combo_decision combo_dispatch(combo_latch_t *l, int func_down, int idx, int pressed){
    struct combo_decision d = { COMBO_NONE, 0, 0 };
    uint16_t bit = (idx >= 0 && idx < 16) ? (uint16_t)(1u << idx) : 0u;
    if (pressed) {
        if (func_down && bit && idx >= COMBO_BTN_T1 && idx <= COMBO_BTN_RWD) {
            l->latched |= bit;
            d.action = combo_action_for(idx);
            d.consumed = 1;
            d.reset_hold = 1;   /* voids the •• power-off hold (clean-hold gate) */
        }
        return d;
    }
    /* release: swallow iff this idx was consumed-as-combo, regardless of func level */
    if (bit && (l->latched & bit)) {
        l->latched &= (uint16_t)~bit;
        d.consumed = 1;   /* action stays COMBO_NONE; nothing re-fires on release */
    }
    return d;
}
