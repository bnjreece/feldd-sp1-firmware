#ifndef LIB_HEADER_H
#define LIB_HEADER_H
#include <stdint.h>
#include "profile.h"   /* NUM_MODES */

/* The librarian's NVS header record (LIB_ID_HEADER, id 1). 4 bytes, UNCHANGED on
 * the wire size: was { version, active, _rsvd[2] }; now { version, mode,
 * active[NUM_MODES] } — the §0 hierarchy needs ONE active index PER mode (a
 * mode-scoped bank remembers its own current profile). The mode moves from the
 * old _rsvd[0] into a named field; the two active bytes occupy the old
 * {active, _rsvd[1]} bytes. A v2/flat header read by this firmware is discarded
 * by the version-bump reseed anyway (Task 3), so there is no in-place migration
 * to get wrong. Pure/freestanding so the accessors stay host-testable. */
struct lib_header {
    uint8_t version;             /* == PROFILE_VERSION when the store is valid */
    uint8_t mode;                /* device mode 0=MIDI..LIB_HEADER_MODE_MAX */
    uint8_t active[NUM_MODES];   /* per-mode WITHIN-bank active index (0..7 each) */
};

/* Highest mode value this header will persist. Mirrors the reserved-personality
 * ceiling that mode.h's mode_led_pattern() renders (MIDI=0, KEYBOARD=1, 2 and 3
 * reserved). A set_mode above this clamps to 0 (MIDI) rather than persisting
 * garbage. Kept as a local literal so this header stays freestanding (host test
 * includes only <stdint.h> via this file) — bump it in lockstep with mode.h if a
 * new personality ships. */
#define LIB_HEADER_MODE_MAX 3u

static inline void lib_header_init(struct lib_header *h, uint8_t version,
                                   uint8_t active0)
{
    h->version = version;
    h->mode    = 0;                          /* MIDI */
    h->active[0] = active0;                  /* MIDI bank active (within) */
    for (int m = 1; m < NUM_MODES; m++) {
        h->active[m] = 0;                    /* every other bank starts at 0 */
    }
}

static inline uint8_t lib_header_mode(const struct lib_header *h)
{
    return h->mode;
}

static inline void lib_header_set_mode(struct lib_header *h, uint8_t mode)
{
    /* Store the RAW mode byte, range-checked. Do NOT collapse nonzero to 1 — that
     * would silently drop a future reserved personality (2/3) down to KEYBOARD.
     * Out-of-range clamps to MIDI (0), matching mode_led_pattern()'s defensive
     * clamp. */
    h->mode = (mode <= LIB_HEADER_MODE_MAX) ? mode : 0u;   /* raw, range-checked */
}

static inline uint8_t lib_header_active(const struct lib_header *h, uint8_t mode)
{
    return (mode < NUM_MODES) ? h->active[mode] : 0u;
}

static inline void lib_header_set_active(struct lib_header *h, uint8_t mode,
                                         uint8_t within)
{
    if (mode < NUM_MODES) {
        h->active[mode] = within;
    }
}

/* PLAY-role global byte (Feature 4). Stored in a SEPARATE NVS record
 * (LIB_ID_SETTINGS), never in this 4-byte header. 0 = shift (default), 1 = assignable. */
#define LIB_PLAYROLE_DEFAULT 0u
#define LIB_PLAYROLE_MAX     1u
static inline int lib_playrole_valid(uint8_t v){ return v <= LIB_PLAYROLE_MAX; }
static inline uint8_t lib_playrole_load(int present, uint8_t stored){
    return (present && lib_playrole_valid(stored)) ? stored : LIB_PLAYROLE_DEFAULT;
}

/* Feature B (0.23): persisted LED brightness level. 0 = dim (default), 1 = full.
 * Stored in LIB_ID_SETTINGS byte [1] (byte [0] is play_mode). A legacy 1-byte
 * record or a missing record decodes as the default, so no device is reseeded. */
#define LIB_BRIGHTNESS_DEFAULT 0u
static inline int     lib_brightness_valid(uint8_t v){ return v <= 1u; }
static inline uint8_t lib_brightness_load(int present, uint8_t stored){
    return (present && lib_brightness_valid(stored)) ? stored : LIB_BRIGHTNESS_DEFAULT;
}

#endif /* LIB_HEADER_H */
