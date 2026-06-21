/*
 * kbd_hid.c — Keyboard-mode held-key state + pure boot-keyboard report builder.
 *
 * See kbd_hid.h for the contract and the auto-repeat rationale. This file is pure
 * C (no Zephyr / USB) so test_kbd_hid.c can exercise it on the host: a button held
 * across many scans keeps its key asserted; release clears it; two held buttons
 * report two keycodes; a held modifier stays asserted.
 */
#include "kbd_hid.h"

#include <string.h>

void kbd_state_reset(struct kbd_state *s)
{
    if (!s) {
        return;
    }
    memset(s, 0, sizeof(*s));
}

void kbd_state_press(struct kbd_state *s, int idx, uint8_t modifiers, uint8_t keycode)
{
    if (!s || idx < 0 || idx >= KBD_NUM_BUTTONS) {
        return;
    }
    s->held[idx] = 1;
    s->mod[idx]  = modifiers;
    s->key[idx]  = keycode;
}

void kbd_state_release(struct kbd_state *s, int idx)
{
    if (!s || idx < 0 || idx >= KBD_NUM_BUTTONS) {
        return;
    }
    s->held[idx] = 0;
    /* Leave key/mod as-is; held=0 is what gates them out of the report. */
}

int kbd_state_any_held(const struct kbd_state *s)
{
    if (!s) {
        return 0;
    }
    for (int i = 0; i < KBD_NUM_BUTTONS; i++) {
        if (s->held[i]) {
            return 1;
        }
    }
    return 0;
}

void kbd_build_report(const struct kbd_state *s, uint8_t out[KBD_REPORT_LEN])
{
    memset(out, 0, KBD_REPORT_LEN);
    if (!s) {
        return;
    }

    uint8_t mods = 0;
    uint8_t keys[KBD_MAX_KEYS];
    int nkeys = 0;
    int overflow = 0;

    for (int i = 0; i < KBD_NUM_BUTTONS; i++) {
        if (!s->held[i]) {
            continue;
        }
        /* Held modifiers always stay asserted, even for a modifier-only key. */
        mods |= s->mod[i];

        uint8_t k = s->key[i];
        if (k == 0) {
            continue;   /* unbound / modifier-only: no keycode slot */
        }
        /* Collapse duplicates (same usage on two held buttons -> one slot). */
        int dup = 0;
        for (int j = 0; j < nkeys; j++) {
            if (keys[j] == k) {
                dup = 1;
                break;
            }
        }
        if (dup) {
            continue;
        }
        if (nkeys < KBD_MAX_KEYS) {
            keys[nkeys++] = k;
        } else {
            overflow = 1;   /* more than 6 distinct keys held */
        }
    }

    out[0] = mods;   /* byte 1 (reserved) already zeroed by memset */

    if (overflow) {
        /* Boot-protocol NKRO overflow: fill all 6 keycode slots with 0x01
         * (ErrorRollOver), per the HID spec, while still reporting modifiers. */
        for (int j = 0; j < KBD_MAX_KEYS; j++) {
            out[2 + j] = 0x01;
        }
    } else {
        for (int j = 0; j < nkeys; j++) {
            out[2 + j] = keys[j];
        }
    }
}
