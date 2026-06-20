#ifndef SP1_WDT_H
#define SP1_WDT_H

/* Reload (feed) the hardware watchdog. Defined in main.c.
 *
 * Any long SYNCHRONOUS boot work that runs BEFORE the control loop starts its
 * per-tick feed must call this so it cannot trip the ~8 s WDT. The motivating
 * case (audit finding C10, 2026-06-18): on the first boot after a PROFILE_VERSION
 * bump, librarian seed_defaults() re-seeds all 8 profiles + forces an NVS
 * garbage-collection erase cycle — 8 writes + a flash erase, with no feed of its
 * own — stacked behind USB enumeration. Feeding across that work keeps the seed
 * comfortably inside the watchdog window and avoids a first-boot reset/boot-loop. */
void feed_wdt(void);

#endif /* SP1_WDT_H */
