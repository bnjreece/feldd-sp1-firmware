#ifndef LIB_BANK_H
#define LIB_BANK_H
#include <stdint.h>
#include "profile.h"   /* NUM_BANK_PROFILES, NUM_MODES, NUM_PROFILES */

/* lib_bank.h — the ONE definition of the mode>profile>layer hierarchy's
 * index arithmetic (spec §0, LOCKED 2026-06-19). Pure + freestanding (only
 * <stdint.h> via profile.h) so the bank math is host-tested (test_lib_bank.c)
 * the same way lib_header.h / seed_cadence.h split their pure cores out of the
 * Zephyr-bound librarian.c.
 *
 * MODEL: profiles are MODE-SCOPED BANKS. There are NUM_MODES banks of
 * NUM_BANK_PROFILES each (2 x 8 = NUM_PROFILES = 16 NVS slots). The active MODE
 * selects a bank; each mode remembers its own WITHIN-bank active index (0..7).
 *
 *   global slot index  = bank(mode) + within        (0..15, the NVS slot id)
 *   within (per-mode)  = global % NUM_BANK_PROFILES  (0..7, what the UI shows)
 *
 * librarian_active_index() returns the WITHIN index (0..7); the global index is
 * an internal NVS detail produced by lib_bank_global(). */

/* The global base slot of `mode`'s bank: MIDI(0)->0, KEYBOARD(1)->8. */
static inline uint8_t lib_bank_of(uint8_t mode)
{
    return (uint8_t)(mode * NUM_BANK_PROFILES);
}

/* The global NVS slot (0..15) a (mode, within) pair addresses. */
static inline uint8_t lib_bank_global(uint8_t mode, uint8_t within)
{
    return (uint8_t)(lib_bank_of(mode) + within);
}

/* The within-bank index (0..7) of a global slot — drops the bank. */
static inline uint8_t lib_bank_within(uint8_t global)
{
    return (uint8_t)(global % NUM_BANK_PROFILES);
}

/* Advance the within-bank active index, wrapping inside the bank of 8 (never
 * crossing into the neighbouring bank — that is what a mode flip is for). */
static inline uint8_t lib_bank_cycle(uint8_t within)
{
    return (uint8_t)((within + 1u) % NUM_BANK_PROFILES);
}

/* Coerce a possibly-corrupt within index back into 0..7 (a clamp, not a wrap):
 * any value >= NUM_BANK_PROFILES is treated as a corrupt header and reset to 0. */
static inline uint8_t lib_bank_clamp(uint8_t within)
{
    return (within < NUM_BANK_PROFILES) ? within : 0u;
}

#endif /* LIB_BANK_H */
