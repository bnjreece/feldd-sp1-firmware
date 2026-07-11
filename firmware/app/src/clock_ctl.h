#ifndef CLOCK_CTL_H
#define CLOCK_CTL_H
#include <stdint.h>
/* clock_ctl - pure clock-source mode selector for the MIDI clock feature.
 * Picks exactly one of two modes: GEN (we generate the clock, default/fallback)
 * or THRU (an external master is present, we pass its clock through). Works in
 * microsecond timestamps supplied by the caller. Feeding it each incoming 0xF8
 * flips it to THRU and measures the tick interval; a >2 s gap with no ticks means
 * the master vanished, so it falls back to GEN, keeping the last measured
 * interval so the generator can resume at that tempo. Pure; host-testable. */

enum clock_mode { CLK_MODE_GEN = 0, CLK_MODE_THRU = 1 };

struct clock_ctl {
	enum clock_mode mode;   /* current source: GEN or THRU (never both) */
	uint8_t  ext_present;   /* 1 while an external master is being heard */
	uint32_t last_ext_us;   /* timestamp of the most recent 0xF8 */
	uint32_t prev_ext_us;   /* timestamp of the 0xF8 before that */
	uint32_t ext_ivl_us;    /* last measured tick interval (us) */
	uint8_t  have_prev;     /* 1 once we have seen at least one tick */
};

/* Reset to defaults: mode GEN, ext_present 0, all counters 0. */
void clock_ctl_init(struct clock_ctl *c);

/* Call on each incoming EXTERNAL real-time clock byte (0xF8). Marks the master
 * present (ext_present=1, mode=THRU) and, once a previous tick exists, records
 * the measured interval for a seamless BPM handoff. */
void clock_ctl_ext_clock(struct clock_ctl *c, uint32_t now_us);

/* Call periodically. If the master is present but its last tick is older than
 * 2,000,000 us it is considered gone: ext_present=0, mode=GEN (ext_ivl_us kept).
 * Returns the resulting mode. */
enum clock_mode clock_ctl_service(struct clock_ctl *c, uint32_t now_us);

/* Last measured external tick interval, in microseconds. */
uint32_t clock_ctl_ext_ivl_us(const struct clock_ctl *c);

#endif
