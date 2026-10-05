#ifndef CONTROLS_H
#define CONTROLS_H
int controls_init(void);
int controls_read_raw(int idx);   /* idx 0..6 per zephyr_user; -1 on error, else 0..4095 */
/* One snapshot of the control channels 0..5 (both ladders, four faders) per
 * control pass: a single SAADC sequence, two samplings averaged -- one driver
 * round trip instead of one per channel per sample. 0 ok, <0 on error (the
 * previous snapshot is kept). controls_snap(idx) reads it: idx 0..5, else -1. */
int controls_sample(void);
int controls_snap(int idx);
#endif
