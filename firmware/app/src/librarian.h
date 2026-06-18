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
 *   id 1            -> header { uint8_t version, active, _rsvd[2]; }
 *   id 0x100 + n    -> struct profile  (n in 0..NUM_PROFILES-1)
 */
#define NUM_PROFILES 8

int  librarian_init(void);                                 /* mount NVS, first-boot defaults, load active */
const struct profile *librarian_active(void);              /* RAM hot copy (fast path, never hits flash) */
uint8_t librarian_active_index(void);
int  librarian_read(uint8_t n, struct profile *out);       /* 0 ok, <0 error */
int  librarian_write(uint8_t n, const struct profile *in); /* 0 ok; persists; refreshes RAM if n is active */
int  librarian_set_active(uint8_t n);                      /* 0 ok; persists index + reloads RAM */
int  librarian_reset(uint8_t n);                           /* 0 ok; reseed slot n to factory default, persist, refresh RAM if active */
int  librarian_reset_all(void);                            /* 0 ok; reseed all slots to factory default, persist, refresh RAM */

#endif
