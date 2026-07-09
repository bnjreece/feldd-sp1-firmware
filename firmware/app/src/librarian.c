/*
 * librarian.c — NVS-backed profile librarian (M5.2)
 *
 * Persists NUM_PROFILES profiles + an active index in the storage_partition
 * (app.overlay, flash 0xF7000, 32 KB — top of usable flash, just under the
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
#include "nvs_erase.h"
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

/* v9 NVS BUDGET GUARD (spec §3). The storage_partition is 0x8000 (32 KB) = 8 pages
 * of 4 KB. NVS reserves ONE page for garbage collection, so usable = 7 pages =
 * 28,672 B. Each persisted profile costs sizeof(struct profile) + an 8-byte NVS
 * Allocation Table Entry (ATE); the header costs sizeof(struct lib_header) + ATE. The
 * whole working set must fit with comfortable GC headroom (the naive full-12B-chord-
 * per-button design was ~8% free and was REJECTED; chord6 lands at ~30% free).
 *
 * usable      = (8 pages - 1 GC) * 4096                         = 28672
 * occupancy   = NUM_PROFILES * (sizeof(profile) + 8 ATE)
 *             + (sizeof(lib_header) + 8 ATE)
 *             = 16 * (1038 + 8) + (4 + 8) = 16748
 * byte-sum headroom = 28672 - 16748 = 11924, but that is NOT the safety proof
 * (NVS cannot straddle a sector). At 3 profiles/sector * 7 usable sectors = 21
 * slots there are ~5 spare entry-slots (~24%) and ~1 fully-free sector, so an
 * edit forces near-immediate GC (no ENOSPC at v9). Storability is PROVEN by the
 * sector-packing assert below.
 *
 * Derived from FIXED_PARTITION_SIZE + the real page size, NOT hardcoded, so a flash-
 * map change re-checks this. NVS_ATE = 8 is the nRF52 NVS entry footer size. */
#define SP1_NVS_PAGE_SIZE   4096u
#define SP1_NVS_GC_PAGES    1u
#define SP1_NVS_ATE         8u
#define SP1_NVS_USABLE \
    (((FIXED_PARTITION_SIZE(storage_partition) / SP1_NVS_PAGE_SIZE) - SP1_NVS_GC_PAGES) \
     * SP1_NVS_PAGE_SIZE)
#define SP1_NVS_OCCUPANCY \
    ((unsigned)NUM_PROFILES * (sizeof(struct profile) + SP1_NVS_ATE) \
     + (sizeof(struct lib_header) + SP1_NVS_ATE))
BUILD_ASSERT(SP1_NVS_USABLE == 28672u,
             "NVS usable budget must be 28672 B (32 KB partition, 8 pages, 1 GC reserve)");
BUILD_ASSERT(SP1_NVS_OCCUPANCY <= SP1_NVS_USABLE,
             "16 v9 profiles + header must fit the usable NVS budget");
/* headroom >= ~25% of usable: keep a real GC margin (chord6 lands at ~30%). */
BUILD_ASSERT((SP1_NVS_USABLE - SP1_NVS_OCCUPANCY) * 4u >= SP1_NVS_USABLE,
             "v9 NVS headroom must stay >= 25% of usable (GC safety)");

/* Sector-packing storability proof (spec §3.4). The byte-sum OCCUPANCY <= USABLE
 * above is NECESSARY BUT NOT SUFFICIENT: Zephyr NVS cannot straddle a sector, so
 * each 4096-B sector (minus 2 close/GC ATEs = 4080 usable) packs only
 * floor(4080 / (sizeof(profile) + ATE)) WHOLE entries and wastes the remainder.
 * At v9: (4096-16)/(1038+8) = 3 profiles/sector, so 16 profiles need
 * ceil(16/3) = 6 sectors; with 1 of 8 reserved for GC, 7 are usable (6 <= 7).
 * This is also the future-bump tripwire: a field bump past ~1360 B/profile
 * (2/sector) keeps the byte-sum green while real capacity drops to 9 > 8, which
 * would ENOSPC at the seed_defaults() nvs_write on a debuggerless unit. */
#define SP1_NVS_SECTOR_COUNT \
    (FIXED_PARTITION_SIZE(storage_partition) / SP1_NVS_PAGE_SIZE)
#define SP1_NVS_ENTRIES_PER_SECTOR \
    ((SP1_NVS_PAGE_SIZE - 2u * SP1_NVS_ATE) / (sizeof(struct profile) + SP1_NVS_ATE))
BUILD_ASSERT(DIV_ROUND_UP(NUM_PROFILES, SP1_NVS_ENTRIES_PER_SECTOR)
                 + SP1_NVS_GC_PAGES <= SP1_NVS_SECTOR_COUNT,
             "NVS must physically store NUM_PROFILES entries plus a GC sector");

/* NVS entry ids. Header is a low fixed id; profiles live in a 0x100+n block so
 * they never collide with the header or any future small bookkeeping ids. */
#define LIB_ID_HEADER        1u
#define LIB_ID_SETTINGS      2u   /* future-bookkeeping band 2..0xFF; holds play_mode */
#define LIB_ID_PROFILE_BASE  0x100u

static struct nvs_fs fs;
static bool          fs_ready;

/* In-RAM hot copies. active_profile is what the control loop reads. The active
 * profile of mode m lives at GLOBAL slot lib_bank_global(m, active_within[m]). */
static struct profile active_profile;
static uint8_t        active_within[NUM_MODES];  /* per-mode WITHIN-bank active (0..7 each) */
static uint8_t        active_mode;               /* current device mode (fast path) */
static uint8_t        play_mode_cache;            /* Feature 4: 0 shift, 1 assignable */

/* Build slot `slot`'s default profile into *p.
 *
 * v6 fader-CC layout — one CLEAN GLOBAL RUNNING COUNTER so every default fader
 * slot gets a UNIQUE CC across the whole 128-fader surface, zero overlap. For
 * profile P (within-bank index 0..7), layer L (0..3: L1/L2/L3/L4), fader F (0..3):
 *
 *     fader_cc = P*16 + L*4 + F      (P*(NUM_LAYERS*NUM_FADERS) + L*NUM_FADERS + F)
 *
 * That runs 0..127 across all 8*4*4 = 128 fader slots, each value unique, max 127
 * (P7L3F3 = 127). 0-indexed: the very first default fader is CC0 (intentional).
 * This replaced the old "4 base + 4 shift" +8/profile scheme, which advanced the
 * base by +8/profile while every profile now fills 16 faders (4 layers x 4), so
 * P0 = CC 1-16 and P1 restarted at CC 9 — a heavy redundant overlap (Peter).
 *
 * Buttons (0.12.1 "stable, non-conflicting default"): split the default across
 * THREE MIDI channels so a fader and a button can NEVER collide on the same
 * (channel, CC) pair — the conflict Peter hit (a CC button defaulting onto a
 * fader's CC on ch1 clobbered the fader; e.g. profile 0 L1 button[0] value 0 ==
 * fader[0] CC 0). Faders own MIDI ch1 (channel 0); the 4 FRONT track buttons own
 * ch2 (channel 1); the 4 SIDE buttons own ch3 (channel 2). PLAY (idx 0) is the
 * gesture button and never emits, so it stays on the profile channel. button
 * index map (buttons.h): 0=Play, 1..4=Track1..4 (front), 5..8=Vol+/Vol-/FWD/RWD
 * (side). Each button GROUP gets the SAME clean within*16 + L*4 + slot 0..127
 * spread the faders use, so ch2 (front) and ch3 (side) each cover CC 0..127 with
 * no overlap — three full unique surfaces (128 faders + 128 front + 128 side).
 * See default_btn_channel()/default_btn_value() below.
 *
 * channel 0; faders linear 0..127; name "Default". Keyboard layers
 * (button_key/mod on every layer) default unbound (0, via the memset). */

/* Default per-button MIDI channel: FRONT track buttons (idx 1..4) -> MIDI ch2
 * (channel 1), SIDE buttons (idx 5..8) -> MIDI ch3 (channel 2), PLAY (idx 0)
 * stays on the profile channel (ch1 / 0). This is what keeps faders (ch1) and
 * buttons from ever sharing a (channel, CC). */
static uint8_t default_btn_channel(int i)
{
    if (i >= 1 && i <= 4) return 1;   /* front track buttons -> MIDI ch2 */
    if (i >= 5 && i <= 8) return 2;   /* side buttons        -> MIDI ch3 */
    return 0;                          /* PLAY                -> profile channel */
}

/* Default per-button CC value for layer L (0..3) of within-bank profile `within`
 * (0..7). Each GROUP (front idx 1..4, side idx 5..8) gets its own 0..3 slot and
 * the SAME within*16 + L*4 + slot spread the faders use, so on ch2 the 128 front
 * buttons and on ch3 the 128 side buttons each cover CC 0..127 uniquely. PLAY
 * (idx 0) emits nothing -> value 0. */
static uint8_t default_btn_value(int within, int L, int i)
{
    int base = within * (NUM_LAYERS * NUM_FADERS);   /* was literal 16; now 32 at 8 layers */
    if (i >= 1 && i <= 4) return (uint8_t)((base + L * NUM_FADERS + (i - 1)) & 0x7F); /* front slot 0..3 */
    if (i >= 5 && i <= 8) return (uint8_t)((base + L * NUM_FADERS + (i - 5)) & 0x7F); /* side  slot 0..3 */
    return 0;                                                                          /* PLAY */
}

/* 0.19: three Teenage Engineering starter profiles seeded on MIDI-bank slots 2/3/4
 * (OP-XY, TX-6, OP-1 field). Same maps feldd.com ships as browser templates, verified
 * against each device's official MIDI reference. A fresh flash / factory reset seeds
 * them; existing users keep their profiles (flashing never reseeds). Slot 0 (Default)
 * + slot 1 stay the generic non-conflicting layout; the Keyboard bank is untouched. */
struct te_seed {
    uint8_t channel;
    uint8_t fcc[NUM_FADERS];     /* L1 fader CCs */
    uint8_t fchan[NUM_FADERS];   /* per-fader channel */
    uint8_t scc[NUM_FADERS];     /* L2 (shift) fader CCs, 0 = unbound */
    uint8_t btype[NUM_BUTTONS];  /* button types (enum btn_type) */
    uint8_t bval[NUM_BUTTONS];   /* button values */
    uint8_t bchan[NUM_BUTTONS];  /* per-button channel */
};

static void apply_te_seed(struct profile *p, const struct te_seed *s)
{
    p->channel = s->channel;
    for (int i = 0; i < NUM_FADERS; i++) {
        p->fader[i].cc = s->fcc[i];
        p->fader[i].min = 0;
        p->fader[i].max = 127;
        p->fader[i].curve = CURVE_LINEAR;
        p->fader[i].invert = 0;
        p->fader_channel[i]     = s->fchan[i];
        p->shift.fader_cc[i]    = s->scc[i];
        p->layer[0].fader_cc[i] = 0;   /* L3 unbound */
        p->layer[1].fader_cc[i] = 0;   /* L4 unbound */
    }
    for (int i = 0; i < NUM_BUTTONS; i++) {
        p->button[i].type  = s->btype[i];
        p->button[i].value = s->bval[i];
        p->button_channel[i]        = s->bchan[i];
        p->shift.button_value[i]    = 0;
        p->layer[0].button_value[i] = 0;
        p->layer[1].button_value[i] = 0;
    }
}

/* v9: OP-XY 8-track. L1..L8 = tracks 1..8 on MIDI ch 0..7. Same sound-design set on
 * every layer; only the channel (= track = layer) changes. Faders CC32/33/31/38;
 * T1 mute CC9 / T2 send-to-tape CC37 / T3 porta CC29 / T4 FX-II CC39 are PER-TRACK
 * (ride the layer channel); Vol+/- CC83/84 and rocker CC104/105 are any-channel
 * globals (ride the layer channel harmlessly). Verified vs
 * feldd-sp-1/docs/te-midi-cc/opxy.md (CC37 send-to-tape ch1-8 confirmed :54). */
struct te_seed8 {
    uint8_t nlayers;                /* = 8 */
    uint8_t fcc[NUM_FADERS];        /* fader CCs, same on every layer */
    uint8_t btype[NUM_BUTTONS];     /* button types, same on every layer */
    uint8_t bval[NUM_BUTTONS];      /* button values, same on every layer */
};

static const struct te_seed8 SEED_OPXY8 = {
    .nlayers = 8,
    .fcc   = { 32, 33, 31, 38 },
    .btype = { BTN_NONE, BTN_CC_TOGGLE, BTN_CC_MOMENTARY, BTN_CC_TOGGLE,
               BTN_CC_MOMENTARY, BTN_CC_MOMENTARY, BTN_CC_MOMENTARY,
               BTN_CC_MOMENTARY, BTN_CC_MOMENTARY },
    .bval  = { 0, 9, 37, 29, 39, 83, 84, 104, 105 },
};

/* Write layer L (0..NUM_LAYERS-1) with fader CCs / button types+values, ALL on MIDI
 * channel `chan`, faders linear 0..127. Routes the heterogeneous per-layer storage
 * (L0 inline, L1 shift+ext[0], L2..L7 layer[L-2]+ext[L-1]) behind one call. Same
 * inline/shift/layer[L-2] routing the generic make_default seed loop uses. */
static void set_layer(struct profile *p, int L, uint8_t chan,
                      const uint8_t fcc[NUM_FADERS],
                      const uint8_t btype[NUM_BUTTONS],
                      const uint8_t bval[NUM_BUTTONS])
{
    for (int i = 0; i < NUM_FADERS; i++) {
        if (L == 0) { p->fader[i].cc=fcc[i]; p->fader[i].min=0; p->fader[i].max=127;
            p->fader[i].curve=CURVE_LINEAR; p->fader[i].invert=0; p->fader_channel[i]=chan; }
        else if (L == 1) { p->shift.fader_cc[i]=fcc[i]; p->ext[0].fader_min[i]=0;
            p->ext[0].fader_max[i]=127; p->ext[0].fader_curve[i]=CURVE_LINEAR;
            p->ext[0].fader_invert[i]=0; p->ext[0].fader_channel[i]=chan; }
        else { p->layer[L-2].fader_cc[i]=fcc[i]; p->ext[L-1].fader_min[i]=0;
            p->ext[L-1].fader_max[i]=127; p->ext[L-1].fader_curve[i]=CURVE_LINEAR;
            p->ext[L-1].fader_invert[i]=0; p->ext[L-1].fader_channel[i]=chan; }
    }
    for (int i = 0; i < NUM_BUTTONS; i++) {
        if (L == 0) { p->button[i].type=btype[i]; p->button[i].value=bval[i];
            p->button_channel[i]=chan; }
        else if (L == 1) { p->shift.button_value[i]=bval[i]; p->ext[0].button_type[i]=btype[i];
            p->ext[0].button_channel[i]=chan; }
        else { p->layer[L-2].button_value[i]=bval[i]; p->ext[L-1].button_type[i]=btype[i];
            p->ext[L-1].button_channel[i]=chan; }
    }
}

static void apply_te_seed8(struct profile *p, const struct te_seed8 *s)
{
    p->channel = 0;                                   /* profile default = track 1 */
    for (int L = 0; L < s->nlayers && L < NUM_LAYERS; L++)
        set_layer(p, L, (uint8_t)L, s->fcc, s->btype, s->bval);   /* channel = L = track L+1 */
}

/* TX-6 (per-track ch1-6): faders track 1-4 volume (CC7); T1-Vol- mute tracks 1-6
 * (CC120 ch1-6); FWD start/stop (CC46 ch7); RWD open. The •• dial switches feldd
 * profiles. No shift. */
static const struct te_seed SEED_TX6 = {
    .channel = 0,
    .fcc   = { 7, 7, 7, 7 },
    .fchan = { 0, 1, 2, 3 },
    .scc   = { 0, 0, 0, 0 },
    .btype = { BTN_NONE, BTN_CC_TOGGLE, BTN_CC_TOGGLE, BTN_CC_TOGGLE,
               BTN_CC_TOGGLE, BTN_CC_TOGGLE, BTN_CC_TOGGLE,
               BTN_CC_MOMENTARY, BTN_NONE },
    .bval  = { 0, 120, 120, 120, 120, 120, 120, 46, 0 },
    .bchan = { 0, 0, 1, 2, 3, 4, 5, 6, 0 },
};

/* OP-1 field (ch1, fw 1.7.0+): faders synth params 1-4 (CC46-49); shift env ADSR
 * (CC50-53); T1-T4 notes C4/E4/G4/C5, Vol+ synth/drum mode (CC93), Vol- sustain
 * (CC64), rocker FWD/RWD tape play/stop (CC105/104). The •• dial switches feldd
 * profiles, so the rocker is free for transport. */
static const struct te_seed SEED_OP1 = {
    .channel = 0,
    .fcc   = { 46, 47, 48, 49 },
    .fchan = { 0, 0, 0, 0 },
    .scc   = { 50, 51, 52, 53 },
    .btype = { BTN_NONE, BTN_NOTE, BTN_NOTE, BTN_NOTE, BTN_NOTE,
               BTN_CC_TOGGLE, BTN_CC_MOMENTARY, BTN_CC_MOMENTARY,
               BTN_CC_MOMENTARY },
    .bval  = { 0, 60, 64, 67, 72, 93, 64, 105, 104 },
    .bchan = { 0, 0, 0, 0, 0, 0, 0, 0, 0 },
};

static void make_default(int slot, struct profile *p)
{
    memset(p, 0, sizeof(*p));
    p->version = PROFILE_VERSION;
    p->channel = 0;

    /* Within-bank index 0..7 (the •• cycle). Both banks reuse the same 0..127
     * counter slice; only one profile is active at a time, so reuse is harmless. */
    int within = slot % NUM_BANK_PROFILES;
    int fbase  = within * (NUM_LAYERS * NUM_FADERS);   /* within*32 (0,32,...,224) */
    /* v9: loop every layer 0..NUM_LAYERS-1 (was unrolled to L0..L3). Mask to 7 bits so
     * no default CC exceeds 127 (which would fail profile_validate and loop the reseed).
     * The counter wraps 0..255 across 256 slots, so each CC 0..127 lands exactly twice. */
    for (int L = 0; L < NUM_LAYERS; L++) {
        for (int i = 0; i < NUM_FADERS; i++) {
            uint8_t cc = (uint8_t)((fbase + L * NUM_FADERS + i) & 0x7F);
            if (L == 0) {
                p->fader[i].cc     = cc;
                p->fader[i].min    = 0;
                p->fader[i].max    = 127;
                p->fader[i].curve  = CURVE_LINEAR;
                p->fader[i].invert = 0;
                p->fader_channel[i] = p->channel;   /* v2: default to the profile channel */
            } else if (L == 1) {
                p->shift.fader_cc[i] = cc;
            } else {
                p->layer[L - 2].fader_cc[i] = cc;
            }
        }
    }
    for (int L = 0; L < NUM_LAYERS; L++) {
        for (int i = 0; i < NUM_BUTTONS; i++) {
            uint8_t bv = default_btn_value(within, L, i);
            if (L == 0) {
                /* Feature 4: seed PLAY (idx 0) as silent BTN_NONE so an
                 * un-reserved/promoted PLAY does not emit the CC#0 Bank-Select
                 * placeholder that collides with fader[0]. default_btn_value(...,0)
                 * already returns 0 for idx 0 and default_btn_channel(0)==0, so
                 * PLAY stays fully silent until explicitly mapped. Matches what
                 * SEED_OPXY8/SEED_TX6/SEED_OP1 already do for idx 0; BTN_NONE still
                 * passes profile_validate. */
                p->button[i].type    = (i == 0) ? BTN_NONE : BTN_CC_MOMENTARY;
                p->button[i].value   = bv;
                p->button_channel[i] = default_btn_channel(i); /* faders ch1 / front ch2 / side ch3 */
            } else if (L == 1) {
                p->shift.button_value[i] = bv;
            } else {
                p->layer[L - 2].button_value[i] = bv;
            }
        }
    }
    BUILD_ASSERT(BTN_NONE == 0, "PLAY seed relies on BTN_NONE == 0");

    /* 0.19: on MIDI-bank slots 1/2/3 (profiles 2/3/4), replace the generic map with a
     * TE starter profile, so they sit right after profile 1 (the Default) with no gap.
     * Applied BEFORE the ext inheritance below so L2/L3/L4 page fields pick up the
     * seed's per-control channels + ranges. Slot 0 + slots 4..7 + Keyboard bank stay
     * generic. */
    if (slot == 2) {
        apply_te_seed(p, &SEED_TX6);
    } else if (slot == 3) {
        apply_te_seed(p, &SEED_OP1);
    }

    /* v6: seed each appended ext bank (L2/L3/L4) to INHERIT L1's per-fader
     * min/max/curve/invert, L1's button type, and L1's per-control channels. v5
     * shared these from L1, so inheriting them keeps the v5 "every layer looks
     * like L1" behavior and makes every layer a complete, sensible default (faders
     * with a real 0..127 range, not min=max=0). Only the per-layer fader CCs +
     * button values + keyboard binds (seeded above / left unbound) differ. */
    for (int L = 0; L < NUM_LAYERS - 1; L++) {       /* ext[0]=L2, [1]=L3, [2]=L4 */
        for (int i = 0; i < NUM_FADERS; i++) {
            p->ext[L].fader_min[i]     = p->fader[i].min;
            p->ext[L].fader_max[i]     = p->fader[i].max;
            p->ext[L].fader_curve[i]   = p->fader[i].curve;
            p->ext[L].fader_invert[i]  = p->fader[i].invert;
            p->ext[L].fader_channel[i] = p->fader_channel[i];
        }
        for (int i = 0; i < NUM_BUTTONS; i++) {
            p->ext[L].button_type[i]    = p->button[i].type;
            p->ext[L].button_channel[i] = p->button_channel[i];
        }
    }

    /* v9 OP-XY 8-track: apply LAST so its per-layer ext[] channels 0..7 survive the
     * inheritance loop above (which would otherwise copy L1's ch0 over ext[1..7] and
     * collapse the mixer to a single track). This inverts the old apply-before-inherit
     * order the single-track seeds still use. */
    if (slot == 1) {
        apply_te_seed8(p, &SEED_OPXY8);
    }

    /* v8: no chords seeded by default (every chord6 slot all-zero, every fader_role =
     * cc - all from the memset above). Chord velocity defaults to 100 (softer than a
     * plain note's 127, user-configurable without a version bump). */
    p->chord_flags[0] = 100;

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
    } else if (slot == 1) {
        nm = "OP-XY";
    } else if (slot == 2) {
        nm = "TX-6";
    } else if (slot == 3) {
        nm = "OP-1 field";
    }

    /* name[16], NUL-padded by the memset above. */
    memcpy(p->name, nm, strlen(nm));
}

/* Adapters so fs_bring_up's mixed-geometry erase drives the pure, host-tested
 * nvs_erase_sweep() cadence (nvs_erase.h): feed the WDT before every sector
 * erase. ctx is the opened flash_area; `sector` is 0..sector_count-1. */
static void erase_feed_cb(void *ctx)
{
    (void)ctx;
    feed_wdt();
}
static int erase_sector_cb(void *ctx, int sector)
{
    const struct flash_area *fa = ctx;
    return flash_area_erase(fa, (uint32_t)sector * (uint32_t)fs.sector_size,
                            (uint32_t)fs.sector_size);
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
        /* Mount failed over a possibly-mixed geometry (the v9 0xFB000 -> 0xF7000
         * move; lower 4 sectors are stale app-region flash). A stock DFU rewrites
         * only the app image and never this partition, so a bad-mount state is
         * deterministic and survives every re-flash; the only clear is a full
         * erase (spec §3.7, MANDATORY). Erase every sector, feeding the WDT before
         * each via the host-tested nvs_erase_sweep() cadence so the 32 KB erase
         * can't trip the ~8 s watchdog, then retry the mount ONCE. */
        const struct flash_area *fa;
        if (flash_area_open(FIXED_PARTITION_ID(storage_partition), &fa) == 0) {
            (void)nvs_erase_sweep((int)fs.sector_count, erase_feed_cb,
                                  erase_sector_cb, (void *)fa);
            flash_area_close(fa);
        }
        rc = nvs_mount(&fs);
        if (rc) {
            return rc;
        }
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

    /* Feature 4: PLAY role lives in its OWN NVS record (LIB_ID_SETTINGS), never in
     * the 4-byte header. A device that has never toggled it (-ENOENT) defaults to 0
     * (shift); a present-but-invalid byte also falls back to the default. This is a
     * pure ADD, so no existing device is short-read/reseeded. */
    uint8_t pr = 0;
    ssize_t rpr = nvs_read(&fs, LIB_ID_SETTINGS, &pr, sizeof(pr));
    play_mode_cache = lib_playrole_load(rpr == (ssize_t)sizeof(pr), pr);

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

uint8_t librarian_play_mode(void)
{
    return play_mode_cache;   /* RAM copy — never hits flash */
}

int librarian_set_play_mode(uint8_t v)
{
    /* Feature 4: PLAY role. Writes ONLY the id-2 record; the 4-byte lib_header is
     * never touched (so no short-read reseed/wipe). Range-checked to 0/1; a no-op
     * same-value set does not burn an NVS write. */
    if (!fs_ready) {
        return -EINVAL;
    }
    if (!lib_playrole_valid(v)) {
        return -EINVAL;
    }
    if (v == play_mode_cache) {
        return 0;             /* no-op: don't burn an NVS write */
    }
    ssize_t w = nvs_write(&fs, LIB_ID_SETTINGS, &v, sizeof(v));
    if (w < 0) {
        return (int)w;
    }
    play_mode_cache = v;
    return 0;
}
