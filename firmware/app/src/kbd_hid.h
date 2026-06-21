#ifndef KBD_HID_H
#define KBD_HID_H
#include <stdint.h>

/* Keyboard-mode HELD-key state + pure report builder (Studio: Peter auto-repeat).
 *
 * THE BUG IT FIXES: usb_hid_send_key() queued a press IMMEDIATELY followed by a
 * release — a momentary TAP — so a held button never held its key, and the host
 * OS's native typematic auto-repeat never engaged. A real USB keyboard just keeps
 * the key asserted in the report while the physical key is down; the OS does the
 * repeating. This module models exactly that: it tracks which of the 9 buttons
 * are currently held in Keyboard mode and the (mod,key) each had AT ITS PRESS
 * edge, then builds the standard 8-byte boot-keyboard report from that held set.
 *
 * It is a PURE, host-testable surface (no Zephyr, no USB). main.c owns the edges:
 * on a keyboard-button press it records (idx -> mod,key); on a release it clears
 * idx; either way it rebuilds the report and sends it raw (usb_hid_send_report,
 * no auto-release). Releasing the last key yields the all-zero report (key-up).
 *
 * WHY (mod,key) IS LATCHED AT PRESS: the active layer can change between a key's
 * press and its release (a layer/peek gesture). Latching what the button emitted
 * when it went down keeps the held key stable and guarantees the matching key-up
 * clears the SAME slot — no stuck keys if the keymap shifts mid-hold. */

#define KBD_NUM_BUTTONS 9   /* mirrors profile.h NUM_BUTTONS / buttons.h BTN_COUNT */
#define KBD_REPORT_LEN  8   /* boot-keyboard: [mod][rsvd][k0..k5] */
#define KBD_MAX_KEYS    6   /* keycode slots in a boot report (rollover ceiling)  */

/* Per-button held record. held=0 means the button is up (key/mod ignored). */
struct kbd_state {
    uint8_t held[KBD_NUM_BUTTONS];   /* 1 while the physical button is down      */
    uint8_t key[KBD_NUM_BUTTONS];    /* HID usage latched at the press edge       */
    uint8_t mod[KBD_NUM_BUTTONS];    /* HID modifier bitmask latched at press     */
};

/* Reset to all-up (no keys held). Call at init and whenever leaving Keyboard
 * mode so a later return starts clean (no phantom holds). */
void kbd_state_reset(struct kbd_state *s);

/* Press edge: latch this button's (mod,key) and mark it held. idx out of range
 * is ignored. A key of 0 (unbound) is still recorded as held but contributes no
 * keycode to the report (its modifier, if any, still applies — same as a real
 * keyboard's modifier-only key). */
void kbd_state_press(struct kbd_state *s, int idx, uint8_t modifiers, uint8_t keycode);

/* Release edge: mark this button up. idx out of range is ignored. */
void kbd_state_release(struct kbd_state *s, int idx);

/* Build the 8-byte boot-keyboard report from the current held set into out[8]:
 *   out[0] = OR of every held button's modifier bitmask
 *   out[1] = 0 (reserved)
 *   out[2..7] = up to 6 DISTINCT non-zero held keycodes
 * Duplicate keycodes (same key bound to two held buttons) collapse to one slot.
 * If more than 6 distinct keys are held, the report rolls over: out[2..7] are all
 * set to 0x01 (ErrorRollOver), the standard NKRO-overflow signal, while modifiers
 * are still reported (a real keyboard does this). out must be at least 8 bytes. */
void kbd_build_report(const struct kbd_state *s, uint8_t out[KBD_REPORT_LEN]);

/* Convenience: true if no button is held (report would be all-zero). */
int kbd_state_any_held(const struct kbd_state *s);

#endif
