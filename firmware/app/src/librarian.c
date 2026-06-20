/*
 * librarian.c — NVS-backed profile librarian (M5.2)
 *
 * Persists NUM_PROFILES profiles + an active index in the storage_partition
 * (app.overlay, flash 0xFB000, 16 KB — top of usable flash, just under the
 * bootloader's reserved last page). One in-RAM hot copy of the active
 * profile is kept so the ~8 ms control loop never touches flash: it reads
 * librarian_active() every tick, which returns the cached struct. Flash is
 * touched only at boot (mount + load) and on an explicit edit/switch.
 *
 * NVS ids:
 *   LIB_ID_HEADER (1)         -> struct lib_header { version, mode, active[NUM_MODES]; }
 *   LIB_ID_PROFILE_BASE+g     -> struct profile  (g in 0..NUM_PROFILES-1; bank 0 =
 *                                MIDI [0..7], bank 1 = Keyboard [8..15])
 *
 * First boot (header read returns -ENOENT, or its version != PROFILE_VERSION)
 * writes NUM_PROFILES default profiles + a fresh header, then loads the active
 * profile of the current mode (lib_bank_global(mode, active[mode])).
 *
 * HARDWARE-DEFERRED: flash persistence across a real power-cycle is verified on
 * Unit A later; there is no flash model in Renode. This file is BUILD-VERIFIED
 * (mount/read/write logic compiles + links against the NVS subsystem).
 */

#include <string.h>
#include <errno.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/fs/nvs.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/sys/util.h>
#include "librarian.h"
#include "lib_header.h"
#include "lib_bank.h"
#include "seed_cadence.h"
#include "wdt.h"

/* F1 non-overlap guard: the NVS storage_partition MUST sit above the app region
 * (the linker-capped app image) and below the reserved bootloader page, so a
 * growing app can never collide with persisted profiles and NVS can never write
 * the 0xFF000 page (writing it could brick the TE bootloader, our recovery path).
 *
 * NOTE: NCS Partition Manager is DISABLED for this image (sysbuild.conf:
 * SB_CONFIG_PARTITION_MANAGER=n). The SP-1's flash map is dictated by the fixed
 * external TE bootloader, not by PM, so the devicetree fixed-partitions are
 * authoritative: FIXED_PARTITION_*(storage_partition) resolves to the app.overlay
 * `reg` (offset 0xFB000, size 0x4000, ending exactly at the reserved 0xFF000
 * page). We bound the offset on BOTH sides against the marisko app_partition cap
 * (0xDF000, the app-region end) and the reserved page (0xFF000). A failing
 * BUILD_ASSERT aborts the build, so a clean build proves NVS sits in the safe
 * window [0xDF000, 0xFF000). */
#define SP1_APP_REGION_END   0xDF000u
#define SP1_RESERVED_PAGE    0xFF000u
BUILD_ASSERT(FIXED_PARTITION_OFFSET(storage_partition) >= SP1_APP_REGION_END,
             "NVS storage_partition must sit above the app region (0xDF000)");
BUILD_ASSERT(FIXED_PARTITION_OFFSET(storage_partition) +
                 FIXED_PARTITION_SIZE(storage_partition) <= SP1_RESERVED_PAGE,
             "NVS storage_partition must end at/below the reserved page (0xFF000)");

/* NVS entry ids. Header is a low fixed id; profiles live in a 0x100+n block so
 * they never collide with the header or any future small bookkeeping ids. */
#define LIB_ID_HEADER        1u
#define LIB_ID_PROFILE_BASE  0x100u

static struct nvs_fs fs;
static bool          fs_ready;

/* In-RAM hot copies. active_profile is what the control loop reads. The active
 * profile of mode m lives at GLOBAL slot lib_bank_global(m, active_within[m]). */
static struct profile active_profile;
static uint8_t        active_within[NUM_MODES];  /* per-mode WITHIN-bank active (0..7 each) */
static uint8_t        active_mode;               /* current device mode (fast path) */

/* Build slot `slot`'s default profile into *p. Sequential CC layout so the 8
 * slots give a predictable 64-fader control surface with no cross-slot clashes:
 * faders are CC 8*slot+1..+4 on the base layer and +5..+8 on shift (slot 0 ->
 * CC 1-8, slot 7 -> CC 57-64). Buttons are momentary CC 102-110 (a fixed band
 * above the faders). channel 0; faders linear 0..127; name "Default". */
static void make_default(int slot, struct profile *p)
{
    memset(p, 0, sizeof(*p));
    p->version = PROFILE_VERSION;
    p->channel = 0;

    /* Per-BANK CC numbering (within-bank index): each mode's 8 slots span CC
     * 1..64, so the 16-slot store never overflows 127. The old global-index form
     * (slot * 8 + 1) put slot 15's shift CC at 128 = invalid MIDI CC. Both banks
     * reuse 1..64; only one profile is active at a time, so reuse is harmless. */
    int within = slot % NUM_BANK_PROFILES;
    int base = within * (2 * NUM_FADERS) + 1;   /* within 0 -> CC 1, within 7 -> CC 57 */
    for (int i = 0; i < NUM_FADERS; i++) {
        p->fader[i].cc     = (uint8_t)(base + i);                /* base layer CC */
        p->fader[i].min    = 0;
        p->fader[i].max    = 127;
        p->fader[i].curve  = CURVE_LINEAR;
        p->fader[i].invert = 0;
        p->shift.fader_cc[i] = (uint8_t)(base + NUM_FADERS + i); /* shift = the next 4 */
        p->fader_channel[i]  = p->channel;    /* v2: default to the profile channel */
    }
    for (int i = 0; i < NUM_BUTTONS; i++) {
        p->button[i].type    = BTN_CC_MOMENTARY;
        p->button[i].value   = (uint8_t)(102 + i);     /* fixed band above the faders */
        p->shift.button_value[i] = (uint8_t)(102 + i);
        p->button_channel[i] = p->channel;    /* v2: default to the profile channel */
    }

    /* Keyboard profile 1 (the first slot of the Keyboard bank) ships with a
     * starter keymap so flipping into Keyboard mode is never blank: the "Editor
     * pad" preset (mirrors the web davinciShortcutPad) — T1-4 = J/K/L/I,
     * Vol+/- = O/B, FWD = Cmd+S, RWD = Cmd+Z. Play stays the shift trigger (no
     * key). Other keyboard slots stay unbound for the user to fill. */
    const char *nm = "Default";
    if (slot == NUM_BANK_PROFILES) {
        static const uint8_t kbd1_key[NUM_BUTTONS] =
            { 0x00, 0x0d, 0x0e, 0x0f, 0x0c, 0x12, 0x05, 0x16, 0x1d };
        static const uint8_t kbd1_mod[NUM_BUTTONS] =
            { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x08, 0x08 };
        for (int i = 0; i < NUM_BUTTONS; i++) {
            p->button_key[i] = kbd1_key[i];
            p->button_mod[i] = kbd1_mod[i];
        }
        nm = "Editor pad";
    }

    /* name[16], NUL-padded by the memset above. */
    memcpy(p->name, nm, strlen(nm));
}

/* Bind the NVS fs to the storage_partition and mount. Derives sector_size from
 * the flash page geometry at the partition offset; sector_count tiles the whole
 * partition. Returns 0 on success. */
static int fs_bring_up(void)
{
    struct flash_pages_info info;
    int rc;

    fs.flash_device = FIXED_PARTITION_DEVICE(storage_partition);
    if (!device_is_ready(fs.flash_device)) {
        return -ENODEV;
    }
    fs.offset = FIXED_PARTITION_OFFSET(storage_partition);

    rc = flash_get_page_info_by_offs(fs.flash_device, fs.offset, &info);
    if (rc) {
        return rc;
    }
    fs.sector_size  = info.size;
    fs.sector_count = (uint16_t)(FIXED_PARTITION_SIZE(storage_partition) / info.size);

    rc = nvs_mount(&fs);
    if (rc) {
        return rc;
    }
    fs_ready = true;
    return 0;
}

/* feed/write side effects for seed_cadence_run(). ctx is unused on the device:
 * the NVS fs + the active-index policy (mode = MIDI, active = 0) are statics, and
 * a fresh seed is always the all-defaults case. The cadence (feed before every
 * write + once after the header) lives in seed_cadence.h, NOT here, so the host
 * test exercises the SAME loop and a dropped feed fails CI (audit C10). */
static void seed_feed_cb(void *ctx)
{
    (void)ctx;
    feed_wdt();
}

static int seed_write_cb(void *ctx, int id)
{
    (void)ctx;
    if (id == SEED_CADENCE_HEADER_ID) {
        struct lib_header hdr;
        lib_header_init(&hdr, PROFILE_VERSION, 0);   /* mode = MIDI on a fresh seed */
        return (int)nvs_write(&fs, LIB_ID_HEADER, &hdr, sizeof(hdr));
    }
    struct profile def;
    make_default(id, &def);   /* per-slot sequential CCs */
    return (int)nvs_write(&fs, (uint16_t)(LIB_ID_PROFILE_BASE + id),
                          &def, sizeof(def));
}

/* Write all NUM_PROFILES (16) defaults + a header with mode=MIDI and all bank
 * actives = 0. Used on first boot or when the stored header is missing/
 * incompatible. */
static int seed_defaults(void)
{
    /* This runs synchronously at boot (librarian_init), BEFORE the control loop
     * starts feeding the watchdog. On a PROFILE_VERSION bump the partition is
     * already full of the previous version's entries, so these writes force an
     * NVS garbage-collection erase cycle on top of the writes themselves — a
     * non-trivial, un-fed window stacked behind USB enumeration. The cadence in
     * seed_cadence_run() feeds the WDT before every write and once after the
     * header write so the re-seed can never trip the ~8 s watchdog and cause a
     * first-boot reset / re-seed boot-loop (audit C10, 2026-06-18).
     *
     * test_lib_header.c::t_reseed_feeds_wdt_every_write drives this SAME
     * seed_cadence_run() with counting stubs, so dropping a feed_wdt() (i.e.
     * editing seed_cadence.h) is caught in CI, not on a low cell. */
    return seed_cadence_run(NUM_PROFILES, seed_feed_cb, seed_write_cb, NULL);
}

int librarian_init(void)
{
    int rc = fs_bring_up();
    if (rc) {
        return rc;
    }

    struct lib_header hdr;
    ssize_t r = nvs_read(&fs, LIB_ID_HEADER, &hdr, sizeof(hdr));

    /* Also reseed if the store predates a profile-COUNT growth. The 0.7.x->0.8.0
     * 8->16 bank split is invisible to the version check: 0.7.x's header was
     * {version, active, _rsvd[2]} — same 4 bytes, same version byte 3 — so
     * `hdr.version == PROFILE_VERSION` passes, yet slots 8..15 (the Keyboard bank)
     * were never written, so a `list` (or any keyboard-bank read) returns NVS_FAIL
     * and the old `active`/`_rsvd` bytes get mis-read as mode/active[]. Probe the
     * TOP slot: if it is absent, the NVS is from a smaller layout and must reseed. */
    uint8_t probe;
    int top_slot_missing =
        nvs_read(&fs, LIB_ID_PROFILE_BASE + (NUM_PROFILES - 1u),
                 &probe, sizeof(probe)) < 0;

    if (r != (ssize_t)sizeof(hdr) || hdr.version != PROFILE_VERSION || top_slot_missing) {
        /* First boot (or incompatible/short header / pre-bank-split store): lay
         * down defaults. On a
         * genuine first boot nvs_read returns -ENOENT and leaves `hdr` fully
         * uninitialized; a short read leaves it partially uninitialized. Rebuild
         * the WHOLE header via lib_header_init so the device mode + every bank's
         * active index are deterministic 0 (MIDI / within 0) — NOT the stack
         * garbage that the active_mode/active reads below would otherwise latch
         * into the live USB personality. This matches the header seed_defaults()
         * just wrote to flash. */
        rc = seed_defaults();
        if (rc) {
            return rc;
        }
        lib_header_init(&hdr, PROFILE_VERSION, 0);   /* version, mode=MIDI, all actives=0 */
    }

    /* The header carries the device mode + a per-mode WITHIN-bank active index
     * (active[NUM_MODES]). Load the mode + EVERY bank's remembered active into the
     * RAM model, clamping each against a corrupt header. lib_header_init() above
     * guarantees mode 0 (MIDI) + all actives 0 on a fresh seed. */
    active_mode = lib_header_mode(&hdr);
    for (uint8_t m = 0; m < NUM_MODES; m++) {
        active_within[m] = lib_bank_clamp(lib_header_active(&hdr, m));
    }

    /* Load the active profile of the CURRENT mode into the RAM hot copy. The NVS
     * slot it addresses is the GLOBAL index lib_bank_global(mode, within) (0..15).
     * A reserved personality (active_mode >= NUM_MODES) has no bank yet, so it
     * falls back to MIDI/within 0 — active_within[] is never indexed out of range. */
    uint8_t within = (active_mode < NUM_MODES) ? active_within[active_mode] : 0u;
    uint8_t g      = lib_bank_global((active_mode < NUM_MODES) ? active_mode : 0u,
                                     within);
    struct profile p;
    rc = librarian_read(g, &p);
    if (rc) {
        /* The slot is unreadable: fall back to a default so the loop has a sane
         * profile, but keep the persisted indices. */
        make_default(g, &p);
    }
    active_profile = p;
    return 0;
}

const struct profile *librarian_active(void)
{
    return &active_profile;   /* RAM copy — never hits flash */
}

uint8_t librarian_active_index(void)
{
    /* WITHIN-bank index (0..7) of the current mode — what the •• cycle, the
     * monitor push, the profile_blink cue, and the protocol active fields all
     * key off (semantically "which of THIS mode's 8"). The GLOBAL slot (0..15)
     * is an internal NVS detail produced by lib_bank_global(). A reserved
     * personality has no bank, so report within 0. */
    return (active_mode < NUM_MODES) ? active_within[active_mode] : 0u;
}

int librarian_read(uint8_t g, struct profile *out)
{
    if (!fs_ready || g >= NUM_PROFILES || out == NULL) {   /* g = GLOBAL slot 0..15 */
        return -EINVAL;
    }
    ssize_t r = nvs_read(&fs, (uint16_t)(LIB_ID_PROFILE_BASE + g),
                         out, sizeof(*out));
    if (r != (ssize_t)sizeof(*out)) {
        return (r < 0) ? (int)r : -EIO;
    }
    return 0;
}

int librarian_write(uint8_t g, const struct profile *in)
{
    if (!fs_ready || g >= NUM_PROFILES || in == NULL) {   /* g = GLOBAL slot 0..15 */
        return -EINVAL;
    }
    ssize_t w = nvs_write(&fs, (uint16_t)(LIB_ID_PROFILE_BASE + g),
                          in, sizeof(*in));
    if (w < 0) {
        return (int)w;
    }
    /* If we just rewrote the CURRENT mode's active slot, refresh the RAM hot copy
     * so the control loop sees the edit without a flash read on its fast path.
     * The active slot is the GLOBAL index lib_bank_global(mode, within). */
    if (active_mode < NUM_MODES &&
        g == lib_bank_global(active_mode, active_within[active_mode])) {
        active_profile = *in;
    }
    return 0;
}

int librarian_set_active(uint8_t within)
{
    /* `within` is a WITHIN-bank index (0..7) — it selects one of the CURRENT
     * mode's 8 profiles (the •• cycle). It is NOT a global 0..15 slot; the global
     * slot is recomposed via lib_bank_global(active_mode, within). */
    if (!fs_ready || within >= NUM_BANK_PROFILES) {
        return -EINVAL;
    }

    /* Load the target slot first; only persist once we know the profile reads
     * back, so a bad slot can't strand the active index. */
    uint8_t g = lib_bank_global(active_mode, within);
    struct profile p;
    int rc = librarian_read(g, &p);
    if (rc) {
        return rc;
    }

    /* Persist mode + EVERY bank's remembered active the SAME way librarian_init()
     * reads it back, so the write->read round-trip is self-consistent across a
     * power-cycle (the reader recomposes each bank's slot from active[mode]). Only
     * the CURRENT mode's active changes; the other banks keep their remembered
     * indices. */
    struct lib_header hdr;
    lib_header_init(&hdr, PROFILE_VERSION, 0);   /* zero version + all bank actives */
    lib_header_set_mode(&hdr, active_mode);
    for (uint8_t m = 0; m < NUM_MODES; m++) {
        lib_header_set_active(&hdr, m, active_within[m]);
    }
    lib_header_set_active(&hdr, active_mode, within);   /* the change */
    ssize_t w = nvs_write(&fs, LIB_ID_HEADER, &hdr, sizeof(hdr));
    if (w < 0) {
        return (int)w;
    }

    active_profile             = p;
    active_within[active_mode] = within;
    return 0;
}

int librarian_reset(uint8_t g)
{
    if (!fs_ready || g >= NUM_PROFILES) {   /* g = GLOBAL slot 0..15 */
        return -EINVAL;
    }
    /* Reseed global slot g to its factory default. librarian_write persists it via
     * the NVS path and refreshes the RAM hot copy when g is the active slot. */
    struct profile def;
    make_default(g, &def);
    return librarian_write(g, &def);
}

int librarian_reset_all(void)
{
    if (!fs_ready) {
        return -EINVAL;
    }
    /* Reseed every slot to its per-slot default. librarian_write persists each via
     * NVS and refreshes the RAM hot copy for the active slot. */
    for (uint8_t n = 0; n < NUM_PROFILES; n++) {
        struct profile def;
        make_default(n, &def);
        int rc = librarian_write(n, &def);
        if (rc) {
            return rc;
        }
    }
    return 0;
}

uint8_t librarian_mode(void)
{
    return active_mode;   /* RAM copy — never hits flash */
}

int librarian_set_mode(uint8_t m)
{
    if (!fs_ready) {
        return -EINVAL;
    }
    /* Range-check, don't boolean-collapse: store the raw mode value so a future
     * reserved personality (mode.h: 2/3) persists its real value instead of being
     * silently saved as KEYBOARD (1). An out-of-range value is rejected rather
     * than written, so a bad caller can't corrupt the header. */
    if (m > LIB_HEADER_MODE_MAX) {
        return -EINVAL;
    }
    if (m == active_mode) {
        return 0;          /* no-op: don't burn an NVS write on a same-mode set */
    }

    /* Switch the device to mode `m`. The §0 hierarchy: a mode flip restores that
     * bank's INDEPENDENTLY remembered active profile (NOT within 0). Persist the
     * new mode + EVERY bank's remembered active the SAME way librarian_init()
     * reads it back. */
    struct lib_header hdr;
    lib_header_init(&hdr, PROFILE_VERSION, 0);
    lib_header_set_mode(&hdr, m);
    for (uint8_t mm = 0; mm < NUM_MODES; mm++) {
        lib_header_set_active(&hdr, mm, active_within[mm]);
    }
    ssize_t w = nvs_write(&fs, LIB_ID_HEADER, &hdr, sizeof(hdr));
    if (w < 0) {
        return (int)w;
    }
    active_mode = m;

    /* Re-point the RAM hot copy at the NEW mode's remembered active profile. A
     * reserved personality (m >= NUM_MODES) has no bank yet -> fall back to within
     * 0 so active_within[] is never indexed out of range. */
    uint8_t within = (m < NUM_MODES) ? active_within[m] : 0u;
    uint8_t g      = lib_bank_global((m < NUM_MODES) ? m : 0u, within);
    struct profile p;
    if (librarian_read(g, &p) == 0) {
        active_profile = p;
    } else {
        make_default(g, &p);
        active_profile = p;
    }
    return 0;
}
