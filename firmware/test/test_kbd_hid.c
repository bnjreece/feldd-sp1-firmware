#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "kbd_hid.h"

/* HID usages used in the tests (values per the boot-keyboard usage table). */
#define K_A   0x04
#define K_B   0x05
#define K_C   0x06
#define K_D   0x07
#define K_E   0x08
#define K_F   0x09
#define K_G   0x0A
#define MOD_LSHIFT 0x02
#define MOD_LCTRL  0x01

static void report_of(const struct kbd_state *s, uint8_t out[KBD_REPORT_LEN])
{
    kbd_build_report(s, out);
}

/* THE BUG-FIX INVARIANT: a button held across many scans keeps its key asserted
 * in EVERY report (the OS then auto-repeats). Release clears it (all-zero key-up). */
static void t_hold_across_scans_then_release(void)
{
    struct kbd_state s;
    kbd_state_reset(&s);

    /* press edge: A goes down */
    kbd_state_press(&s, 1, 0, K_A);

    uint8_t r[KBD_REPORT_LEN];
    /* simulate many control-loop scans while the button stays held */
    for (int scan = 0; scan < 50; scan++) {
        report_of(&s, r);
        assert(r[0] == 0);       /* no modifier */
        assert(r[1] == 0);       /* reserved */
        assert(r[2] == K_A);     /* A still asserted */
        assert(r[3] == 0 && r[4] == 0 && r[5] == 0 && r[6] == 0 && r[7] == 0);
        assert(kbd_state_any_held(&s) == 1);
    }

    /* release edge: A comes up -> all-zero key-up */
    kbd_state_release(&s, 1);
    report_of(&s, r);
    uint8_t zero[KBD_REPORT_LEN] = {0};
    assert(memcmp(r, zero, KBD_REPORT_LEN) == 0);
    assert(kbd_state_any_held(&s) == 0);
}

/* Two simultaneously-held buttons report two keycodes; releasing one leaves the
 * other asserted (no all-keys-up while a key is still held = no stuck/dropped). */
static void t_two_held_keys(void)
{
    struct kbd_state s;
    kbd_state_reset(&s);

    kbd_state_press(&s, 1, 0, K_A);
    kbd_state_press(&s, 2, 0, K_B);

    uint8_t r[KBD_REPORT_LEN];
    report_of(&s, r);
    /* both present (order = button index order: A then B) */
    assert(r[2] == K_A && r[3] == K_B);
    assert(r[4] == 0);

    /* release A only -> B remains asserted */
    kbd_state_release(&s, 1);
    report_of(&s, r);
    assert(r[2] == K_B && r[3] == 0);
    assert(kbd_state_any_held(&s) == 1);

    /* release B -> all up */
    kbd_state_release(&s, 2);
    report_of(&s, r);
    uint8_t zero[KBD_REPORT_LEN] = {0};
    assert(memcmp(r, zero, KBD_REPORT_LEN) == 0);
}

/* Held modifiers stay asserted while held, and a modifier-only button (key=0)
 * contributes its modifier but no keycode (like a real Shift key). */
static void t_modifiers(void)
{
    struct kbd_state s;
    kbd_state_reset(&s);

    /* a Shift-only button (modifier, no key) */
    kbd_state_press(&s, 3, MOD_LSHIFT, 0x00);
    uint8_t r[KBD_REPORT_LEN];
    report_of(&s, r);
    assert(r[0] == MOD_LSHIFT);
    assert(r[2] == 0);   /* no keycode from a modifier-only key */

    /* now also hold Ctrl+A on another button -> mods OR together, A asserted */
    kbd_state_press(&s, 4, MOD_LCTRL, K_A);
    report_of(&s, r);
    assert(r[0] == (MOD_LSHIFT | MOD_LCTRL));
    assert(r[2] == K_A);

    /* release the Ctrl+A button -> Shift modifier still held, A gone */
    kbd_state_release(&s, 4);
    report_of(&s, r);
    assert(r[0] == MOD_LSHIFT);
    assert(r[2] == 0);

    /* release the Shift button -> clean */
    kbd_state_release(&s, 3);
    report_of(&s, r);
    assert(r[0] == 0);
    assert(kbd_state_any_held(&s) == 0);
}

/* Same usage bound to two held buttons collapses to a single keycode slot. */
static void t_duplicate_keys_collapse(void)
{
    struct kbd_state s;
    kbd_state_reset(&s);
    kbd_state_press(&s, 1, 0, K_A);
    kbd_state_press(&s, 2, 0, K_A);
    uint8_t r[KBD_REPORT_LEN];
    report_of(&s, r);
    assert(r[2] == K_A && r[3] == 0);   /* one slot, not two */
    /* releasing only one of them keeps A asserted (the other still holds it) */
    kbd_state_release(&s, 1);
    report_of(&s, r);
    assert(r[2] == K_A);
    kbd_state_release(&s, 2);
    report_of(&s, r);
    assert(r[2] == 0);
}

/* More than 6 distinct keys held -> boot-protocol ErrorRollOver (all 6 slots
 * 0x01), modifiers still reported, no out-of-bounds write. */
static void t_rollover(void)
{
    struct kbd_state s;
    kbd_state_reset(&s);
    uint8_t keys[7] = {K_A, K_B, K_C, K_D, K_E, K_F, K_G};
    for (int i = 0; i < 7; i++) {
        kbd_state_press(&s, i, (i == 0) ? MOD_LSHIFT : 0, keys[i]);
    }
    uint8_t r[KBD_REPORT_LEN];
    report_of(&s, r);
    assert(r[0] == MOD_LSHIFT);   /* modifiers survive overflow */
    for (int j = 2; j < 8; j++) {
        assert(r[j] == 0x01);     /* ErrorRollOver in all keycode slots */
    }
}

/* The latch-at-press invariant: the (mod,key) recorded at the PRESS edge is what
 * stays asserted, even if the keymap (layer) would later resolve the same button
 * differently. main.c latches at press; release clears the SAME slot. We model a
 * layer change as: press with the L0 key, then (the button is still held) the app
 * does NOT re-press, so the report keeps the L0 key; a clean release clears it. */
static void t_latch_at_press_no_strand(void)
{
    struct kbd_state s;
    kbd_state_reset(&s);
    kbd_state_press(&s, 1, 0, K_A);   /* pressed under layer 0 -> A */
    uint8_t r[KBD_REPORT_LEN];
    report_of(&s, r);
    assert(r[2] == K_A);
    /* layer changes mid-hold; main.c does not re-latch held buttons, so the report
     * is unchanged (A still held). The matching release clears exactly that key. */
    report_of(&s, r);
    assert(r[2] == K_A);
    kbd_state_release(&s, 1);
    report_of(&s, r);
    assert(r[2] == 0);
    assert(kbd_state_any_held(&s) == 0);
}

/* reset() drops every held key (the mode-flip / leave-Keyboard path) -> all-up. */
static void t_reset_clears_all(void)
{
    struct kbd_state s;
    kbd_state_reset(&s);
    kbd_state_press(&s, 0, MOD_LCTRL, K_A);
    kbd_state_press(&s, 5, 0, K_B);
    assert(kbd_state_any_held(&s) == 1);
    kbd_state_reset(&s);
    assert(kbd_state_any_held(&s) == 0);
    uint8_t r[KBD_REPORT_LEN];
    report_of(&s, r);
    uint8_t zero[KBD_REPORT_LEN] = {0};
    assert(memcmp(r, zero, KBD_REPORT_LEN) == 0);
}

/* Out-of-range / null indices are ignored (defensive — no OOB write/read). */
static void t_bounds_safe(void)
{
    struct kbd_state s;
    kbd_state_reset(&s);
    kbd_state_press(&s, -1, MOD_LSHIFT, K_A);
    kbd_state_press(&s, KBD_NUM_BUTTONS, 0, K_B);
    kbd_state_press(&s, 999, 0, K_C);
    assert(kbd_state_any_held(&s) == 0);
    kbd_state_release(&s, -1);
    kbd_state_release(&s, KBD_NUM_BUTTONS);
    uint8_t r[KBD_REPORT_LEN];
    report_of(&s, r);
    uint8_t zero[KBD_REPORT_LEN] = {0};
    assert(memcmp(r, zero, KBD_REPORT_LEN) == 0);
}

/* Releasing a button that was never pressed is harmless (the FWD/RWD mode-toggle
 * release that falls through to kbd_state_release in main.c). */
static void t_release_unpressed_is_noop(void)
{
    struct kbd_state s;
    kbd_state_reset(&s);
    kbd_state_press(&s, 1, 0, K_A);
    kbd_state_release(&s, 7);   /* idx 7 (FWD) was never pressed into the set */
    uint8_t r[KBD_REPORT_LEN];
    report_of(&s, r);
    assert(r[2] == K_A);        /* A unaffected, no stuck/dropped key */
}

int main(void)
{
    t_hold_across_scans_then_release();
    t_two_held_keys();
    t_modifiers();
    t_duplicate_keys_collapse();
    t_rollover();
    t_latch_at_press_no_strand();
    t_reset_clears_all();
    t_bounds_safe();
    t_release_unpressed_is_noop();
    printf("all kbd_hid tests passed\n");
    return 0;
}
