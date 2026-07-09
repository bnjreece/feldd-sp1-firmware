#include <assert.h>
#include <stdio.h>
#include "lib_bank.h"

/* bank(mode) = mode * NUM_BANK_PROFILES: MIDI->0, KEYBOARD->8. */
static void t_bank_of_each_mode(void)
{
    assert(lib_bank_of(0) == 0);    /* MIDI    -> global base 0  */
    assert(lib_bank_of(1) == 8);    /* KEYBOARD-> global base 8  */
}

/* global(mode, within) = bank(mode) + within: the NVS slot a (mode, within) pair
 * addresses. Spot-check both banks at their ends. */
static void t_global_index_math(void)
{
    assert(lib_bank_global(0, 0) == 0);
    assert(lib_bank_global(0, 7) == 7);
    assert(lib_bank_global(1, 0) == 8);
    assert(lib_bank_global(1, 7) == 15);
}

/* within(global) is the inverse: global % NUM_BANK_PROFILES, dropping the bank. */
static void t_within_is_global_inverse(void)
{
    for (uint8_t g = 0; g < 16; g++) {
        uint8_t mode   = (uint8_t)(g / 8);
        uint8_t within = lib_bank_within(g);
        assert(within == g % 8);
        assert(lib_bank_global(mode, within) == g);   /* round-trip */
    }
}

/* cycle wraps within the bank of 8, never crossing into the other bank. */
static void t_cycle_wraps_within_bank(void)
{
    assert(lib_bank_cycle(0) == 1);
    assert(lib_bank_cycle(6) == 7);
    assert(lib_bank_cycle(7) == 0);   /* wrap, NOT 8 (would jump banks) */
}

/* clamp coerces an out-of-range within-index back into 0..7 (corrupt NVS guard),
 * never into the neighbouring bank. */
static void t_clamp_guards_within_range(void)
{
    assert(lib_bank_clamp(0) == 0);
    assert(lib_bank_clamp(7) == 7);
    assert(lib_bank_clamp(8) == 0);    /* 8 is bank 1's base, NOT a valid within */
    assert(lib_bank_clamp(200) == 0);
}

/* cycle_prev wraps within the bank of 8 the other direction (••+Vol- profile prev),
 * never crossing into the other bank. */
static void t_cycle_prev_wraps_within_bank(void)
{
    assert(lib_bank_cycle_prev(1) == 0);
    assert(lib_bank_cycle_prev(7) == 6);
    assert(lib_bank_cycle_prev(0) == 7);   /* wrap 0 -> top of THIS bank, not bank -1 */
}

int main(void)
{
    t_bank_of_each_mode();
    t_cycle_prev_wraps_within_bank();
    t_global_index_math();
    t_within_is_global_inverse();
    t_cycle_wraps_within_bank();
    t_clamp_guards_within_range();
    printf("all lib_bank tests passed\n");
    return 0;
}
