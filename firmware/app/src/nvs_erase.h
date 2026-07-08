#ifndef NVS_ERASE_H
#define NVS_ERASE_H

/* nvs_erase.h - the ONE definition of fs_bring_up()'s mixed-geometry erase
 * cadence (spec §3.7). Pure + freestanding (no Zephyr, no flash), the same split
 * as seed_cadence.h, so the REAL erase loop runs on the host in CI.
 *
 * When nvs_mount() fails over the v9 mixed geometry (the 0xFB000 -> 0xF7000 move
 * lands the old 4 NVS sectors in the top half of the new 8-sector region), the
 * partition is erased whole and the mount retried once. That 32 KB erase runs
 * BEFORE the control loop starts feeding the ~8 s watchdog, so it MUST feed
 * before every sector erase. Side effects are injected:
 *
 *   - feed(ctx)          -> feed_wdt()          in production
 *   - erase(ctx, sector) -> flash_area_erase()  in production (returns <0 on err)
 *
 * Both librarian.c::fs_bring_up() AND test_nvs_erase.c drive THIS function, so
 * dropping a feed here fails the host test in CI instead of tripping the WDT on a
 * first-boot erase. Returns the first negative erase() result, else 0. */
static inline int nvs_erase_sweep(int sectors,
                                  void (*feed)(void *ctx),
                                  int  (*erase)(void *ctx, int sector),
                                  void *ctx)
{
    for (int s = 0; s < sectors; s++) {
        feed(ctx);                  /* feed_wdt() before each sector erase */
        int e = erase(ctx, s);      /* flash_area_erase(fa, s*sector_size, sector_size) */
        if (e < 0) {
            return e;
        }
    }
    return 0;
}

#endif /* NVS_ERASE_H */
