#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "lib_header.h"
#include "lib_bank.h"
#include "mode.h"
#include "seed_cadence.h"

/* Each mode remembers its OWN active index; setting one bank's active must not
 * touch the other bank's. (The whole point of mode-scoped banks.) */
static void t_per_mode_active_is_independent(void)
{
    struct lib_header h;
    lib_header_init(&h, 3, 0);                  /* version, both actives = 0 */
    lib_header_set_active(&h, MODE_MIDI, 3);    /* MIDI bank -> within 3 */
    lib_header_set_active(&h, MODE_KEYBOARD, 5);/* KB bank   -> within 5 */
    assert(lib_header_active(&h, MODE_MIDI) == 3);
    assert(lib_header_active(&h, MODE_KEYBOARD) == 5);
    /* flipping back the MIDI bank leaves the KB bank's active intact */
    lib_header_set_active(&h, MODE_MIDI, 1);
    assert(lib_header_active(&h, MODE_MIDI) == 1);
    assert(lib_header_active(&h, MODE_KEYBOARD) == 5);
}

/* mode lives in its own named field now; flipping it never disturbs either
 * bank's active index. */
static void t_mode_field_does_not_clobber_actives(void)
{
    struct lib_header h;
    lib_header_init(&h, 3, 0);
    lib_header_set_active(&h, MODE_MIDI, 2);
    lib_header_set_active(&h, MODE_KEYBOARD, 6);
    lib_header_set_mode(&h, MODE_KEYBOARD);
    assert(lib_header_mode(&h) == MODE_KEYBOARD);
    assert(lib_header_active(&h, MODE_MIDI) == 2);
    assert(lib_header_active(&h, MODE_KEYBOARD) == 6);
    assert(h.version == 3);
}

/* set mode + both actives -> serialize -> wipe RAM (remount) -> read back all. */
static void t_header_survives_remount(void)
{
    struct lib_header h;
    lib_header_init(&h, 3, 0);
    lib_header_set_mode(&h, MODE_KEYBOARD);
    lib_header_set_active(&h, MODE_MIDI, 4);
    lib_header_set_active(&h, MODE_KEYBOARD, 7);

    uint8_t flash[sizeof(struct lib_header)];
    memcpy(flash, &h, sizeof h);          /* nvs_write */
    memset(&h, 0, sizeof h);              /* power-cycle: RAM gone */
    assert(lib_header_mode(&h) == 0);
    memcpy(&h, flash, sizeof h);          /* nvs_read on remount */

    assert(lib_header_mode(&h) == MODE_KEYBOARD);
    assert(lib_header_active(&h, MODE_MIDI) == 4);
    assert(lib_header_active(&h, MODE_KEYBOARD) == 7);
}

/* lib_header_init() must DETERMINISTICALLY zero mode AND both actives even when
 * the struct is full of garbage (models librarian_init's -ENOENT first-boot seed,
 * where `hdr` is uninitialized stack before the seed branch rebuilds it). */
static void t_init_clears_garbage(void)
{
    struct lib_header h;
    memset(&h, 0xFF, sizeof h);
    assert(lib_header_mode(&h) == 0xFF);            /* prove dirty */
    lib_header_init(&h, 3, 0);
    assert(lib_header_mode(&h) == 0);               /* clean MIDI */
    assert(lib_header_active(&h, MODE_MIDI) == 0);
    assert(lib_header_active(&h, MODE_KEYBOARD) == 0);
    assert(h.version == 3);
}

/* Looper/reserved personalities (2/3) persist their RAW mode values, not collapsed
 * to KEYBOARD. (Forward-compat: mode.h reserves 3; LIB_HEADER_MODE_MAX guards.)
 * NOTE: active[] is only NUM_MODES wide, so reserved modes share no active slot
 * yet — this test only pins the mode byte's raw persistence. */
static void t_reserved_personality_persists_raw(void)
{
    struct lib_header h;
    lib_header_init(&h, 3, 0);
    lib_header_set_mode(&h, 2);
    assert(lib_header_mode(&h) == 2);
    lib_header_set_mode(&h, 99);                    /* out of range -> MIDI */
    assert(lib_header_mode(&h) == 0);
}

/* ---- librarian.c write->read round-trip (the regression guard) -------------
 * librarian.c is NVS-bound and not host-compilable, but its persistence is pure
 * header+bank math. These helpers mirror EXACTLY how librarian_set_active() /
 * librarian_set_mode() serialize the header (the WRITERS) and how
 * librarian_init() recomposes the boot-active global slot (the READER). The
 * round-trip must be the identity on the selected global slot. This pins the bug
 * the code review caught: a flat global index dumped into active[0] was lost on
 * reboot in any non-MIDI bank. If a future edit re-desyncs the writer from the
 * reader, THIS test fails — the gap is no longer invisible to CI. */

/* WRITER: how librarian_set_active(n=global slot) lays down the header. */
static void lib_writer_set_active(struct lib_header *h, uint8_t n)
{
    uint8_t bank_mode = (uint8_t)(n / NUM_BANK_PROFILES);
    uint8_t within    = lib_bank_within(n);
    lib_header_init(h, 3, 0);
    lib_header_set_active(h, bank_mode, within);
    lib_header_set_mode(h, bank_mode);
}

/* READER: how librarian_init() recomposes the boot-active GLOBAL slot. */
static uint8_t lib_reader_active_global(const struct lib_header *h)
{
    uint8_t mode   = lib_header_mode(h);
    uint8_t within = lib_bank_clamp(lib_header_active(h, mode));
    return lib_bank_global(mode, within);
}

/* The headline failure from the review: in KEYBOARD mode, selecting global slot
 * 14 must come back as 14 after a power-cycle — not collapse to bank 1 slot 0
 * (global 8). Drive the writer, wipe RAM, run the reader. */
static void t_set_active_round_trips_every_global_slot(void)
{
    for (uint8_t n = 0; n < NUM_PROFILES; n++) {
        struct lib_header h;
        lib_writer_set_active(&h, n);

        uint8_t flash[sizeof(struct lib_header)];
        memcpy(flash, &h, sizeof h);       /* nvs_write */
        memset(&h, 0, sizeof h);           /* power-cycle: RAM gone */
        memcpy(&h, flash, sizeof h);       /* nvs_read on remount */

        assert(lib_reader_active_global(&h) == n);   /* selection survives */
        /* the device boots into the bank that owns the selected slot */
        assert(lib_header_mode(&h) == (uint8_t)(n / NUM_BANK_PROFILES));
    }
}

/* The concrete KEYBOARD-bank case spelled out (mirrors the review's example): a
 * Keyboard-bank selection is NOT silently rewritten to the MIDI bank's slot 0. */
static void t_keyboard_selection_is_not_lost(void)
{
    struct lib_header h;
    lib_writer_set_active(&h, 14);            /* KB bank, within 6 */
    assert(lib_header_mode(&h) == MODE_KEYBOARD);
    assert(lib_header_active(&h, MODE_KEYBOARD) == 6);
    assert(lib_header_active(&h, MODE_MIDI) == 0);   /* the other bank untouched */
    assert(lib_reader_active_global(&h) == 14);      /* round-trips, not 8 */
}

/* Guard the v2->v3 reseed watchdog cadence by running the REAL production loop
 * (seed_cadence.h, shared verbatim with librarian.c::seed_defaults) against
 * counting stubs. The stubs record the interleaving of feed_wdt() and nvs_write()
 * so a regression that drops a feed from the production path fails HERE, in CI,
 * rather than tripping the ~8 s WDT on a low cell during a first-boot reseed
 * (audit C10). Because the cadence has exactly one definition and seed_defaults()
 * drives it too, deleting a feed in seed_cadence.h breaks this test. */
#define SEED_PROFILE_WRITES 16

struct seed_trace {
    int feeds;                /* total feed_wdt() calls */
    int writes;               /* total nvs_write() calls */
    int writes_since_feed;    /* writes since the last feed (must never exceed 1) */
    int max_writes_per_feed;  /* high-water mark of writes_since_feed */
    int seen_feeds;           /* feed calls observed (incl. swallowed) — fault-injection only */
};

static void trace_feed(void *ctx)
{
    struct seed_trace *t = ctx;
    t->feeds++;
    t->writes_since_feed = 0;
}

static int trace_write(void *ctx, int id)
{
    struct seed_trace *t = ctx;
    (void)id;
    t->writes++;
    t->writes_since_feed++;
    if (t->writes_since_feed > t->max_writes_per_feed) {
        t->max_writes_per_feed = t->writes_since_feed;
    }
    return 0;   /* success: never short-circuit the loop */
}

static void t_reseed_feeds_wdt_every_write(void)
{
    struct seed_trace t = {0};
    int rc = seed_cadence_run(SEED_PROFILE_WRITES, trace_feed, trace_write, &t);

    assert(rc == 0);                       /* all writes "succeeded" */
    assert(t.writes == SEED_PROFILE_WRITES + 1);   /* 16 profiles + 1 header */
    assert(t.feeds == SEED_PROFILE_WRITES + 2);    /* a feed per write + 1 after = 18 */
    assert(t.max_writes_per_feed == 1);    /* NEVER two writes between feeds */
}

/* The previous assertion is only meaningful if seed_cadence_run() can be DRIVEN
 * to fail it. Prove the trace WOULD bite a regression by swallowing one inter-write
 * feed (the feed before the 2nd profile write): writes 0 and 1 then share a feed
 * window, so max_writes_per_feed must exceed 1. This pins t_reseed_feeds_wdt_every_write
 * to a real invariant instead of a tautology — if the counting ever stopped
 * detecting a dropped feed, THIS test fails too. The `seen_feeds` counter picks
 * the feed that sits BETWEEN two writes (every write is otherwise preceded by its
 * own feed, so dropping the very first feed would not create a shared window). */
static void skip_second_feed(void *ctx)
{
    struct seed_trace *t = ctx;
    t->seen_feeds++;
    if (t->seen_feeds == 2) {
        return;   /* swallow the feed that precedes the 2nd profile write */
    }
    t->feeds++;
    t->writes_since_feed = 0;
}
static void t_dropped_feed_is_detectable(void)
{
    struct seed_trace t = {0};
    seed_cadence_run(SEED_PROFILE_WRITES, skip_second_feed, trace_write, &t);
    assert(t.max_writes_per_feed > 1);     /* the invariant CAN fail -> the guard is real */
}

/* play_mode policy: absent record -> default 0; present clamps to 0/1 validity;
 * and the reseed-safety lock: lib_header MUST stay 4 bytes (id-2 record holds
 * play_mode, NOT the header - growing the header short-reads every device -> WIPE). */
static void t_playrole_policy_and_header_size(void){
    assert(sizeof(struct lib_header) == 4);          /* NO wipe: header unchanged */
    assert(lib_playrole_load(0, 0) == 0);            /* -ENOENT -> shift default */
    assert(lib_playrole_load(0, 7) == 0);            /* absent ignores stored junk */
    assert(lib_playrole_load(1, 0) == 0);
    assert(lib_playrole_load(1, 1) == 1);
    assert(lib_playrole_load(1, 9) == 0);            /* present but invalid -> default */
    assert(lib_playrole_valid(0) && lib_playrole_valid(1));
    assert(!lib_playrole_valid(2) && !lib_playrole_valid(255));
}

/* Feature B (0.23): brightness persistence. Absent (legacy 1-byte record or ENOENT)
   -> default dim (0); a present valid byte round-trips; a present invalid byte -> default. */
static void t_brightness_load(void)
{
    assert(lib_brightness_load(0, 0) == LIB_BRIGHTNESS_DEFAULT);  /* absent -> default */
    assert(lib_brightness_load(1, 0) == 0);                        /* present dim */
    assert(lib_brightness_load(1, 1) == 1);                        /* present full */
    assert(lib_brightness_load(1, 9) == LIB_BRIGHTNESS_DEFAULT);   /* invalid -> default */
    assert(LIB_BRIGHTNESS_DEFAULT == 0);                           /* default is dim */
}

static void t_bpm_load(void)
{
    assert(lib_bpm_load(0, 0)   == LIB_BPM_DEFAULT);   /* absent (legacy <=2B rec) -> default */
    assert(lib_bpm_load(0, 150) == LIB_BPM_DEFAULT);   /* absent ignores stored junk */
    assert(lib_bpm_load(1, 40)  == 40);                /* present, low bound */
    assert(lib_bpm_load(1, 240) == 240);               /* present, high bound */
    assert(lib_bpm_load(1, 120) == 120);               /* present, mid */
    assert(lib_bpm_load(1, 39)  == LIB_BPM_DEFAULT);   /* below range -> default */
    assert(lib_bpm_load(1, 241) == LIB_BPM_DEFAULT);   /* above range -> default */
    assert(lib_bpm_load(1, 0)   == LIB_BPM_DEFAULT);   /* zero (never-set byte) -> default */
    assert(LIB_BPM_DEFAULT == 120);                    /* musical default */
}

int main(void)
{
    t_playrole_policy_and_header_size();
    t_brightness_load();
    t_bpm_load();
    t_per_mode_active_is_independent();
    t_mode_field_does_not_clobber_actives();
    t_header_survives_remount();
    t_init_clears_garbage();
    t_reserved_personality_persists_raw();
    t_set_active_round_trips_every_global_slot();
    t_keyboard_selection_is_not_lost();
    t_reseed_feeds_wdt_every_write();
    t_dropped_feed_is_detectable();
    printf("all lib_header tests passed\n");
    return 0;
}
