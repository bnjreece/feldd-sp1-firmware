#ifndef FLASH_DEFER_H
#define FLASH_DEFER_H
#include <stdbool.h>
#include <stdint.h>

/* flash_defer.h — holding the librarian's settings writes until the SP-1 is quiet.
 *
 * Pure policy (no Zephyr), host-tested by firmware/test/test_flash_defer.c; the
 * librarian owns the only instance and drives it from the main loop.
 *
 * WHY. On the nRF52840 the CPU stops while flash is written or erased. A small
 * NVS write stalls it for a fraction of a millisecond; NVS garbage collection,
 * which runs whenever a sector fills, stalls it for tens of milliseconds or more.
 * The librarian persists a setting the moment it changes -- a profile step on
 * •• + Vol, a mode flip, LED brightness, the debounced BPM -- which is harmless
 * while the firmware only sends MIDI. Anything time-critical running beside it,
 * such as an audio engine rendering into a few milliseconds of output queue,
 * hears that stall as a dropout.
 *
 * SO: a layer that cares registers a "quiet" predicate with the librarian
 * (librarian_set_quiet_fn). While one is registered, those settings change in RAM
 * at once -- the device behaves exactly the same -- and reach flash at the first
 * main-loop pass where the predicate reports quiet, or at power-off. With none
 * registered, which is stock feldd, every write still happens immediately, in the
 * same order and with the same error returns as before.
 *
 * A failed write is put back and retried no sooner than FLASH_DEFER_RETRY_MS
 * later, so a full or faulty store cannot turn into a write every 8 ms. A forced
 * flush (power-off) ignores the backoff: it is the last chance. */

#define FLASH_DEFER_HEADER    (1u << 0)   /* LIB_ID_HEADER: mode + every bank's active */
#define FLASH_DEFER_SETTINGS  (1u << 1)   /* LIB_ID_SETTINGS: play role, brightness, BPM */
#define FLASH_DEFER_RETRY_MS  1000u

struct flash_defer {
    uint8_t  dirty;          /* FLASH_DEFER_* records changed in RAM, not yet in flash */
    bool     backoff;        /* a write failed; hold until retry_at_ms */
    uint32_t retry_at_ms;
};

static inline void flash_defer_init(struct flash_defer *d)
{
    d->dirty = 0u;
    d->backoff = false;
    d->retry_at_ms = 0u;
}

/* A record changed in RAM and is owed to flash. */
static inline void flash_defer_mark(struct flash_defer *d, uint8_t recs)
{
    d->dirty |= recs;
}

/* A record was written by some other path (a write that cannot wait), so it is
 * no longer owed. */
static inline void flash_defer_clear(struct flash_defer *d, uint8_t recs)
{
    d->dirty &= (uint8_t)~recs;
}

static inline bool flash_defer_pending(const struct flash_defer *d)
{
    return d->dirty != 0u;
}

/* The records to write NOW, removed from the owed set; 0 for nothing. Due when
 * quiet, or when forced; a backoff after a failure holds an unforced take until
 * retry_at_ms (wrap-safe). */
static inline uint8_t flash_defer_take(struct flash_defer *d, uint32_t now_ms,
                                       bool quiet, bool force)
{
    if (d->dirty == 0u) {
        return 0u;
    }
    if (!force) {
        if (!quiet) {
            return 0u;
        }
        if (d->backoff && (int32_t)(now_ms - d->retry_at_ms) < 0) {
            return 0u;
        }
    }
    uint8_t recs = d->dirty;
    d->dirty = 0u;
    d->backoff = false;
    return recs;
}

/* A taken record failed to write: owe it again, and back off. */
static inline void flash_defer_failed(struct flash_defer *d, uint8_t recs, uint32_t now_ms)
{
    d->dirty |= recs;
    d->backoff = true;
    d->retry_at_ms = now_ms + FLASH_DEFER_RETRY_MS;
}

#endif /* FLASH_DEFER_H */
