/* Host tests for midi_thru_dest.
 *
 * The bit arithmetic is trivial. What is worth pinning down is the POLICY, and
 * two properties in particular:
 *
 *   - the default state (both switches off) must produce NONE, because the
 *     caller uses that to skip decoding entirely. feldd ships with both off --
 *     it is a control surface, not a MIDI sink -- so this is the path every
 *     device takes on every USB buffer, and a regression here is a per-message
 *     cost on the hot path rather than a visible bug.
 *
 *   - the two switches must be genuinely INDEPENDENT. The whole reason for a
 *     second record in NVS was that a host connected over both USB and BLE needs
 *     to turn exactly one destination off. A refactor that made BLE imply TRS
 *     (or vice versa) would compile, pass a smoke test, and quietly remove the
 *     only reason the feature was built this way.
 */
#include "midi_thru_dest.h"
#include <assert.h>
#include <stdio.h>

static void test_all_four_combinations(void)
{
    assert(midi_thru_dest(0, 0) == MIDI_THRU_DEST_NONE);
    assert(midi_thru_dest(1, 0) == MIDI_THRU_DEST_TRS);
    assert(midi_thru_dest(0, 1) == MIDI_THRU_DEST_BLE);
    assert(midi_thru_dest(1, 1) == (MIDI_THRU_DEST_TRS | MIDI_THRU_DEST_BLE));
}

/* THE IMPORTANT ONE. Neither switch may influence the other's bit. */
static void test_switches_are_independent(void)
{
    for (int trs = 0; trs <= 1; trs++) {
        for (int ble = 0; ble <= 1; ble++) {
            uint8_t d = midi_thru_dest(trs, ble);
            assert(!!(d & MIDI_THRU_DEST_TRS) == trs);
            assert(!!(d & MIDI_THRU_DEST_BLE) == ble);
        }
    }
}

/* The default path must be exactly zero, not merely falsy: usb_midi1.c compares
 * against MIDI_THRU_DEST_NONE to decide whether to decode the event at all. */
static void test_default_is_none_and_skips_work(void)
{
    uint8_t d = midi_thru_dest(0, 0);
    assert(d == 0);
    assert(d == MIDI_THRU_DEST_NONE);
    assert((d & MIDI_THRU_DEST_TRS) == 0);
    assert((d & MIDI_THRU_DEST_BLE) == 0);
}

/* A stored byte from some future firmware must not read as "neither". The
 * librarian range-checks on write, but the loader tolerates what it finds, and
 * treating 2 as off would silently disable a destination the user enabled. */
static void test_any_nonzero_counts_as_on(void)
{
    assert(midi_thru_dest(2, 0)   == MIDI_THRU_DEST_TRS);
    assert(midi_thru_dest(0, 255) == MIDI_THRU_DEST_BLE);
    assert(midi_thru_dest(-1, 7)  == (MIDI_THRU_DEST_TRS | MIDI_THRU_DEST_BLE));
}

/* The two bits must not collide, or one switch would silently drive both. */
static void test_bits_are_distinct(void)
{
    assert(MIDI_THRU_DEST_TRS != MIDI_THRU_DEST_BLE);
    assert((MIDI_THRU_DEST_TRS & MIDI_THRU_DEST_BLE) == 0);
    assert(MIDI_THRU_DEST_NONE == 0);
}

int main(void)
{
    test_all_four_combinations();
    test_switches_are_independent();
    test_default_is_none_and_skips_work();
    test_any_nonzero_counts_as_on();
    test_bits_are_distinct();
    printf("all midi_thru_dest tests passed\n");
    return 0;
}
