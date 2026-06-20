#ifndef SEED_CADENCE_H
#define SEED_CADENCE_H

/* seed_cadence.h — the ONE definition of seed_defaults()'s watchdog-feed cadence.
 *
 * Pure + freestanding (no Zephyr, no flash) the same way lib_header.h split the
 * header accessors and config_cdc_fmt.c split the pure formatting out of
 * config_cdc.c. This lets the REAL seed loop run on the host in CI.
 *
 * The contract (audit C10, 2026-06-18): the v2->v3 PROFILE_VERSION bump reseeds
 * all 8 profiles + the header synchronously at boot, BEFORE the control loop
 * starts feeding the ~8 s hardware watchdog, and forces an NVS garbage-collection
 * erase on top. So the seed MUST feed the WDT before every NVS write and once
 * more after the final header write, and MUST NEVER let two writes share a single
 * feed window. seed_cadence_run() is that loop, with the side effects injected:
 *
 *   - feed(ctx)        -> feed_wdt()              in production
 *   - write(ctx, id)   -> make_default + nvs_write in production (returns <0 err)
 *
 * Both librarian.c::seed_defaults() AND test_lib_header.c drive THIS function, so
 * deleting a feed here (the only place the cadence lives) fails the host test in
 * CI instead of bricking a low cell on a first-boot reseed.
 *
 * Returns the first negative write() result (propagated for early-return), else 0.
 */

/* Sentinel ids passed to write() so a caller can distinguish the 8 profile writes
 * from the trailing header write. Profile n -> n (0..profiles-1); header -> -1
 * collides with nothing because profile ids are non-negative. */
#define SEED_CADENCE_HEADER_ID (-1)

static inline int seed_cadence_run(int profiles,
                                   void (*feed)(void *ctx),
                                   int  (*write)(void *ctx, int id),
                                   void *ctx)
{
    for (int n = 0; n < profiles; n++) {
        feed(ctx);                      /* feed_wdt() before the profile write */
        int w = write(ctx, n);          /* make_default(n) + nvs_write(profile n) */
        if (w < 0) {
            return w;
        }
    }

    feed(ctx);                          /* feed_wdt() before the header write */
    int w = write(ctx, SEED_CADENCE_HEADER_ID);   /* nvs_write(header) */
    if (w < 0) {
        return w;
    }
    feed(ctx);                          /* feed_wdt() after the header write */
    return 0;
}

#endif /* SEED_CADENCE_H */
