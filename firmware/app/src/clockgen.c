/* clockgen.c - pure MIDI-clock tempo math (24 PPQN). No Zephyr, no hardware;
 * host-testable exactly like midi1_codec.c / protocol.c. See clockgen.h. */
#include "clockgen.h"

#define TAP_STALE_US 2000000u   /* gap over this = a fresh tempo, restart */
#define US_PER_MIN   60000000u  /* microseconds per minute (beat math) */

/* Clamp bpm into [40,240]. */
static uint16_t clamp_bpm(uint32_t bpm)
{
    if (bpm < CLOCKGEN_BPM_MIN) return (uint16_t)CLOCKGEN_BPM_MIN;
    if (bpm > CLOCKGEN_BPM_MAX) return (uint16_t)CLOCKGEN_BPM_MAX;
    return (uint16_t)bpm;
}

uint32_t clockgen_tick_us(uint16_t bpm)
{
    uint16_t b = clamp_bpm(bpm);
    return 2500000u / b;            /* 60e6 / (bpm*24) = 2.5e6 / bpm */
}

uint16_t clockgen_us_to_bpm(uint32_t tick_us)
{
    if (tick_us == 0) return (uint16_t)CLOCKGEN_BPM_MIN;   /* div0 guard */
    return clamp_bpm(2500000u / tick_us);
}

uint16_t clockgen_fader_bpm(uint8_t v, uint16_t bpm_min, uint16_t bpm_max)
{
    if (v > 127) v = 127;
    int32_t span = (int32_t)bpm_max - (int32_t)bpm_min;
    int32_t num  = (int32_t)v * span;
    /* round to nearest (half away from zero), works for either sign of span */
    int32_t delta = num >= 0 ? (num + 63) / 127 : -(((-num) + 63) / 127);
    int32_t bpm = (int32_t)bpm_min + delta;
    /* clamp into [min,max] regardless of which bound is larger */
    int32_t lo = bpm_min < bpm_max ? bpm_min : bpm_max;
    int32_t hi = bpm_min < bpm_max ? bpm_max : bpm_min;
    if (bpm < lo) bpm = lo;
    if (bpm > hi) bpm = hi;
    return (uint16_t)bpm;
}

void clock_tap_reset(struct clock_tap *t)
{
    t->prev_us   = 0;
    t->ivl[0] = t->ivl[1] = t->ivl[2] = t->ivl[3] = 0;
    t->n         = 0;
    t->have_prev = 0;
}

uint16_t clock_tap_bpm(struct clock_tap *t, uint32_t now_us)
{
    /* fresh tempo: no prior tap, or the gap is too long to be the same beat */
    if (!t->have_prev || (now_us - t->prev_us) > TAP_STALE_US) {
        t->n         = 0;
        t->prev_us   = now_us;
        t->have_prev = 1;
        return 0;                  /* need a 2nd tap */
    }

    uint32_t ivl = now_us - t->prev_us;
    t->prev_us = now_us;

    /* push interval, keeping the last up-to-4 */
    if (t->n < 4) {
        t->ivl[t->n++] = ivl;
    } else {
        t->ivl[0] = t->ivl[1];
        t->ivl[1] = t->ivl[2];
        t->ivl[2] = t->ivl[3];
        t->ivl[3] = ivl;
    }

    /* average of stored intervals (n<=4, each <=2e6 -> sum fits in uint32) */
    uint32_t sum = 0;
    for (uint8_t i = 0; i < t->n; i++) sum += t->ivl[i];
    uint32_t avg = sum / t->n;
    if (avg == 0) return (uint16_t)CLOCKGEN_BPM_MIN;   /* div0 guard */

    /* a TAP marks a beat, so BPM = 60,000,000 / avg_tap_interval_us */
    return clamp_bpm(US_PER_MIN / avg);
}
