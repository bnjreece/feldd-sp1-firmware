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
 *   LIB_ID_HEADER (1)         -> struct lib_header { version, active, _rsvd[2]; }
 *   LIB_ID_PROFILE_BASE+n     -> struct profile  (n in 0..NUM_PROFILES-1)
 *
 * First boot (header read returns -ENOENT, or its version != PROFILE_VERSION)
 * writes 8 default profiles + a fresh header, then loads profile[active].
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

struct lib_header {
    uint8_t version;     /* == PROFILE_VERSION when the store is valid */
    uint8_t active;      /* active profile index, 0..NUM_PROFILES-1 */
    uint8_t _rsvd[2];    /* reserved/padding for forward-compat */
};

static struct nvs_fs fs;
static bool          fs_ready;

/* In-RAM hot copies. active_profile is what the control loop reads. */
static struct profile active_profile;
static uint8_t        active_index;

/* Build the M5.2 default profile into *p. Identical for every slot:
 * channel 0; faders {7,74,71,76} linear 0..127 non-inverted; buttons
 * momentary CC (20+idx); shift bank mirrors the base bank; name "Default". */
static void make_default(struct profile *p)
{
    memset(p, 0, sizeof(*p));
    p->version = PROFILE_VERSION;
    p->channel = 0;

    static const uint8_t fader_cc[NUM_FADERS] = { 7, 74, 71, 76 };
    for (int i = 0; i < NUM_FADERS; i++) {
        p->fader[i].cc     = fader_cc[i];
        p->fader[i].min    = 0;
        p->fader[i].max    = 127;
        p->fader[i].curve  = CURVE_LINEAR;
        p->fader[i].invert = 0;
        p->shift.fader_cc[i] = fader_cc[i];   /* shift mirrors base */
    }
    for (int i = 0; i < NUM_BUTTONS; i++) {
        p->button[i].type    = BTN_CC_MOMENTARY;
        p->button[i].value   = (uint8_t)(20 + i);
        p->shift.button_value[i] = (uint8_t)(20 + i);  /* shift mirrors base */
    }

    /* name[16], NUL-padded by the memset above. */
    const char *nm = "Default";
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

/* Write all 8 defaults + a header with active=0. Used on first boot or when the
 * stored header is missing/incompatible. */
static int seed_defaults(void)
{
    struct profile def;
    make_default(&def);

    for (uint8_t n = 0; n < NUM_PROFILES; n++) {
        ssize_t w = nvs_write(&fs, (uint16_t)(LIB_ID_PROFILE_BASE + n),
                              &def, sizeof(def));
        if (w < 0) {
            return (int)w;
        }
    }

    struct lib_header hdr = { .version = PROFILE_VERSION, .active = 0,
                              ._rsvd = { 0, 0 } };
    ssize_t w = nvs_write(&fs, LIB_ID_HEADER, &hdr, sizeof(hdr));
    if (w < 0) {
        return (int)w;
    }
    return 0;
}

int librarian_init(void)
{
    int rc = fs_bring_up();
    if (rc) {
        return rc;
    }

    struct lib_header hdr;
    ssize_t r = nvs_read(&fs, LIB_ID_HEADER, &hdr, sizeof(hdr));
    if (r != (ssize_t)sizeof(hdr) || hdr.version != PROFILE_VERSION) {
        /* First boot (or incompatible/short header): lay down defaults. */
        rc = seed_defaults();
        if (rc) {
            return rc;
        }
        hdr.version = PROFILE_VERSION;
        hdr.active  = 0;
    }

    if (hdr.active >= NUM_PROFILES) {
        hdr.active = 0;     /* corrupt index -> clamp to a valid slot */
    }

    /* Load the active profile into the RAM hot copy. */
    struct profile p;
    rc = librarian_read(hdr.active, &p);
    if (rc) {
        /* The slot is unreadable: fall back to a default so the loop has a
         * sane profile, but keep the persisted index. */
        make_default(&p);
    }
    active_profile = p;
    active_index   = hdr.active;
    return 0;
}

const struct profile *librarian_active(void)
{
    return &active_profile;   /* RAM copy — never hits flash */
}

uint8_t librarian_active_index(void)
{
    return active_index;
}

int librarian_read(uint8_t n, struct profile *out)
{
    if (!fs_ready || n >= NUM_PROFILES || out == NULL) {
        return -EINVAL;
    }
    ssize_t r = nvs_read(&fs, (uint16_t)(LIB_ID_PROFILE_BASE + n),
                         out, sizeof(*out));
    if (r != (ssize_t)sizeof(*out)) {
        return (r < 0) ? (int)r : -EIO;
    }
    return 0;
}

int librarian_write(uint8_t n, const struct profile *in)
{
    if (!fs_ready || n >= NUM_PROFILES || in == NULL) {
        return -EINVAL;
    }
    ssize_t w = nvs_write(&fs, (uint16_t)(LIB_ID_PROFILE_BASE + n),
                          in, sizeof(*in));
    if (w < 0) {
        return (int)w;
    }
    /* If we just rewrote the active slot, refresh the RAM hot copy so the
     * control loop sees the edit without a flash read on its fast path. */
    if (n == active_index) {
        active_profile = *in;
    }
    return 0;
}

int librarian_set_active(uint8_t n)
{
    if (!fs_ready || n >= NUM_PROFILES) {
        return -EINVAL;
    }

    /* Load the target slot first; only persist the new index once we know the
     * profile reads back, so a bad slot can't strand the active index. */
    struct profile p;
    int rc = librarian_read(n, &p);
    if (rc) {
        return rc;
    }

    struct lib_header hdr = { .version = PROFILE_VERSION, .active = n,
                              ._rsvd = { 0, 0 } };
    ssize_t w = nvs_write(&fs, LIB_ID_HEADER, &hdr, sizeof(hdr));
    if (w < 0) {
        return (int)w;
    }

    active_profile = p;
    active_index   = n;
    return 0;
}

int librarian_reset(uint8_t n)
{
    if (!fs_ready || n >= NUM_PROFILES) {
        return -EINVAL;
    }
    /* Reseed slot n to the M5.2 factory default. librarian_write persists it via
     * the NVS path and refreshes the RAM hot copy when n is the active slot. */
    struct profile def;
    make_default(&def);
    return librarian_write(n, &def);
}

int librarian_reset_all(void)
{
    if (!fs_ready) {
        return -EINVAL;
    }
    /* Reseed every slot. librarian_write persists each via NVS and refreshes the
     * RAM hot copy for the active slot, so the control loop sees the reset. */
    struct profile def;
    make_default(&def);
    for (uint8_t n = 0; n < NUM_PROFILES; n++) {
        int rc = librarian_write(n, &def);
        if (rc) {
            return rc;
        }
    }
    return 0;
}
