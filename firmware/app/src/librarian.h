#ifndef LIBRARIAN_H
#define LIBRARIAN_H
#include "profile.h"

/* M5.2 NVS-backed profile librarian.
 *
 * Stores NUM_PROFILES profiles + an "active index" in the NVS storage_partition
 * (app.overlay, flash 0xFB000, ends at the reserved 0xFF000 bootloader page).
 * One in-RAM hot copy of the active profile is
 * kept so the 8 ms control loop never touches flash on its fast path; flash is
 * only read at boot and written on an explicit edit/switch.
 *
 * NVS layout:
 *   id 1            -> header { uint8_t version, mode, active[NUM_MODES]; }
 *   id 0x100 + g    -> struct profile  (g in 0..NUM_PROFILES-1; bank 0 = MIDI
 *                      [0..7], bank 1 = Keyboard [8..15])
 */
/* NUM_PROFILES / NUM_BANK_PROFILES / NUM_MODES are defined in profile.h (the §0
 * mode-scoped-bank constants), pulled in via the include above. */

int  librarian_init(void);                                 /* mount NVS, first-boot defaults, load active */
const struct profile *librarian_active(void);              /* active profile of the CURRENT mode (RAM hot copy,
                                                            * = slot lib_bank_global(mode, active[mode]); never hits flash) */
uint8_t librarian_active_index(void);                      /* WITHIN-bank index (0..7) of the current mode */
int  librarian_read(uint8_t g, struct profile *out);       /* g = GLOBAL slot 0..15; 0 ok, <0 error */
int  librarian_write(uint8_t g, const struct profile *in); /* g = GLOBAL slot 0..15; 0 ok; persists; refreshes RAM if g is the active slot */
int  librarian_set_active(uint8_t within);                 /* set the CURRENT mode's active (within 0..7); persists all actives + mode, reloads RAM */
int  librarian_reset(uint8_t g);                           /* g = GLOBAL slot 0..15; reseed to factory default, persist, refresh RAM if active */
int  librarian_reset_all(void);                            /* 0 ok; reseed all 16 slots to factory default, persist, refresh RAM */
uint8_t librarian_mode(void);                              /* device mode: 0=MIDI (default), 1=KEYBOARD, 2..3 reserved personalities (mode.h) */
int  librarian_set_mode(uint8_t m);                        /* switch bank -> reload that mode's remembered active profile; persists raw mode (0..LIB_HEADER_MODE_MAX); -EINVAL if out of range */
uint8_t librarian_play_mode(void);                         /* Feature 4: PLAY role 0=shift (default), 1=assignable. -ENOENT -> 0; read from the SEPARATE id-2 NVS record, never the header */
int  librarian_set_play_mode(uint8_t v);                   /* persist PLAY role to the id-2 record ONLY (no header write, no wipe); range 0/1, -EINVAL out of range; same-value set is a no-op */
uint8_t librarian_brightness(void);        /* Feature B: 0 dim, 1 full */
int     librarian_set_brightness(uint8_t v);
uint8_t librarian_midi_thru(void);         /* MIDI thru USB->TRS: 0 off (default), 1 on; own record LIB_ID_MIDI_THRU */
int     librarian_set_midi_thru(uint8_t v);
uint8_t librarian_trs_mode(void);          /* TRS jack role: 0 MIDI (default), 1 trigger, 2 sync; own record */
int     librarian_set_trs_mode(uint8_t v);
uint8_t librarian_trs_chan(void);          /* trigger match channel; 0 = omni (default), 1..16 = channel */
int     librarian_set_trs_chan(uint8_t v);
uint8_t librarian_trs_width(void);         /* pulse width in 100 us units, 1..255 (default 100 = 10 ms) */
int     librarian_set_trs_width(uint8_t v);
uint8_t librarian_trs_div(void);           /* SYNC divider: clock ticks per pulse, 1..24 (default 12 = 2 PPQN) */
int     librarian_set_trs_div(uint8_t v);
uint8_t librarian_bpm(void);               /* clock: persisted global GEN tempo 40..240 */
int     librarian_set_bpm(uint8_t v);      /* validate/cache/persist; no-op if unchanged */

#ifdef CONFIG_FELDD_BT_PROVISION
/* Q5 community-provisioning bookkeeping. Grows the LIB_ID_SETTINGS record st[3]->st[6]:
 * st[3] = provision-done flag, st[4]/st[5] = the flashed app major/minor. Compiled ONLY
 * into the FELDD_BT_PROVISION build (the shipped image's 3-byte record is byte-unchanged),
 * and every reader short-reads defensively (rst >= N) so an older 3-byte record still
 * decodes. This flag records INTENT/version only — it NEVER authorizes a flash; the live
 * app-mode READY probe is the sole authority (design references/15 §49). */
uint8_t librarian_provision_done(void);
int     librarian_set_provision_done(uint8_t done, uint8_t app_maj, uint8_t app_min);
#endif

#endif
