/* test_nvs_erase.c - host guard for fs_bring_up()'s mixed-geometry erase WDT
 * cadence. fs_bring_up() is NVS/flash-bound and not host-compilable, but its
 * cadence is pure: erase every sector, feeding the WDT before each, so the 32 KB
 * (8-sector) erase can never trip the ~8 s watchdog. nvs_erase_sweep() is that
 * loop with the side effects injected, driven by BOTH librarian.c (production)
 * and this test - the exact seed_cadence.h / t_reseed_feeds_wdt_every_write
 * pattern. Dropping a feed here fails CI, not a first-boot erase on a low cell. */
#include <assert.h>
#include <stdio.h>
#include "nvs_erase.h"

#define ERASE_SECTORS 8   /* 32 KB partition / 4 KB sector */

struct erase_trace {
    int erases;
    int feeds;
    int erases_since_feed;
    int max_erases_per_feed;
    int seen_feeds;
};

static void trace_feed(void *ctx)
{
    struct erase_trace *t = ctx;
    t->feeds++;
    t->erases_since_feed = 0;
}

static int trace_erase(void *ctx, int sector)
{
    struct erase_trace *t = ctx;
    (void)sector;
    t->erases++;
    t->erases_since_feed++;
    if (t->erases_since_feed > t->max_erases_per_feed) {
        t->max_erases_per_feed = t->erases_since_feed;
    }
    return 0;
}

static void t_erase_sweep_feeds_wdt_every_sector(void)
{
    struct erase_trace t = {0};
    int rc = nvs_erase_sweep(ERASE_SECTORS, trace_feed, trace_erase, &t);
    assert(rc == 0);
    assert(t.erases == ERASE_SECTORS);      /* every sector erased */
    assert(t.feeds  == ERASE_SECTORS);      /* one feed before each erase */
    assert(t.max_erases_per_feed == 1);     /* NEVER two erases between feeds */
}

/* Prove the trace WOULD bite a dropped feed (non-tautology guard, mirrors
 * test_lib_header.c::skip_second_feed): swallow the feed before the 2nd sector
 * so sectors 0 and 1 share a feed window; max_erases_per_feed must exceed 1. */
static void skip_second_feed(void *ctx)
{
    struct erase_trace *t = ctx;
    t->seen_feeds++;
    if (t->seen_feeds == 2) {
        return;                              /* swallow: no reset -> shared window */
    }
    t->erases_since_feed = 0;
}

static void t_erase_sweep_trace_bites_dropped_feed(void)
{
    struct erase_trace t = {0};
    int rc = nvs_erase_sweep(ERASE_SECTORS, skip_second_feed, trace_erase, &t);
    assert(rc == 0);
    assert(t.max_erases_per_feed > 1);
}

static void noop_feed(void *ctx) { (void)ctx; }
static int erase_fail(void *ctx, int sector) { (void)ctx; (void)sector; return -5; }

static void t_erase_sweep_propagates_error(void)
{
    /* first negative erase() return short-circuits (mirrors seed_cadence_run) */
    assert(nvs_erase_sweep(ERASE_SECTORS, noop_feed, erase_fail, NULL) == -5);
}

int main(void)
{
    t_erase_sweep_feeds_wdt_every_sector();
    t_erase_sweep_trace_bites_dropped_feed();
    t_erase_sweep_propagates_error();
    printf("test_nvs_erase: OK\n");
    return 0;
}
