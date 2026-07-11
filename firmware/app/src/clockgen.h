#ifndef CLOCKGEN_H
#define CLOCKGEN_H
#include <stdint.h>
/* clockgen.h - pure MIDI-clock tempo math (24 PPQN). No Zephyr, no hardware;
 * host-testable exactly like midi1_codec / protocol.
 *
 * A MIDI clock runs at 24 pulses per quarter note (PPQN). The tick period is
 * 60,000,000 us/min / (bpm * 24) = 2,500,000 / bpm. A TAP, by contrast, marks a
 * whole quarter note (a beat), so tap BPM = 60,000,000 / tap_interval_us - do
 * not confuse the two intervals. All BPM values are clamped to 40..240. */

#define CLOCKGEN_BPM_MIN 40u
#define CLOCKGEN_BPM_MAX 240u

/* Microseconds between 24-PPQN clock ticks = 2500000 / bpm, bpm clamped 40..240. */
uint32_t clockgen_tick_us(uint16_t bpm);

/* Inverse of clockgen_tick_us: 2500000 / tick_us, clamped 40..240. Returns 40
 * when tick_us is 0 (div0 guard). */
uint16_t clockgen_us_to_bpm(uint32_t tick_us);

/* Linear map of v (0..127) to bpm_min..bpm_max, rounded, clamped to that range. */
uint16_t clockgen_fader_bpm(uint8_t v, uint16_t bpm_min, uint16_t bpm_max);

/* Tap-tempo state: rolling average of the last up-to-4 tap intervals. */
struct clock_tap {
    uint32_t prev_us;   /* timestamp of the previous tap */
    uint32_t ivl[4];    /* last up-to-4 tap intervals (us) */
    uint8_t  n;         /* how many intervals stored (0..4) */
    uint8_t  have_prev; /* 1 once a first tap has been seen */
};

/* Clear all tap state. */
void clock_tap_reset(struct clock_tap *t);

/* Record a tap at now_us and return the current tap BPM.
 * If there was no previous tap, or the gap since the previous tap is
 * > 2,000,000 us (a fresh tempo), the interval history is reset and 0 is
 * returned (need a 2nd tap). Otherwise the interval (now_us - prev_us) is pushed
 * (last 4 kept) and BPM = 60,000,000 / avg_interval, clamped 40..240. prev_us
 * updates on every tap. */
uint16_t clock_tap_bpm(struct clock_tap *t, uint32_t now_us);

#endif
