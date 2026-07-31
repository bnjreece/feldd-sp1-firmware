/*
 * clock_timer.c - internal 96-PPQN musical clock / 24-PPQN MIDI clock.
 *
 * Uses a dedicated nRF hardware TIMER (timer2 @ 1 MHz, 1 us resolution) via the
 * Zephyr counter API, NOT k_timer: k_timer is quantised to the 32768 Hz system tick
 * (~30 us), which is a visible fraction of a short high-BPM tick period and jitters
 * the tempo above ~100 BPM. The counter's periodic top-value callback runs in the
 * timer's own ISR and emits 0xF8 with 1 us-scale precision.
 */
#include "clock_timer.h"
#include "clockgen.h"
#include "midi_out.h"
#include <zephyr/device.h>
#include <zephyr/drivers/counter.h>

#define MIDI_RT_CLOCK 0xF8u

static const struct device *const clk_ctr = DEVICE_DT_GET(DT_NODELABEL(timer2));
static bool running;
static uint16_t cur_bpm;         /* last BPM applied to the timer; 0 = none yet */
static uint8_t midi_clock_phase;
static clock_timer_subtick_fn subtick_listener;
static void *subtick_listener_ctx;
static volatile bool gen_emit = true;  /* GEN emits ticks; THRU clears this so the free-running
                                        * generator stays silent while an external master is passed
                                        * through - gating the ISR emit avoids a start/stop race. */

/* Runs at 96 PPQN. Notify the musical scheduler every time, and emit standard
 * 24-PPQN MIDI Clock on every fourth subtick. Tiny + ISR-safe. */
static void tick_top(const struct device *dev, void *user_data)
{
    ARG_UNUSED(dev);
    ARG_UNUSED(user_data);
    if (subtick_listener) {
        subtick_listener(subtick_listener_ctx);
    }
    if (midi_clock_phase == 0 && gen_emit) {
        midi_out_rt(MIDI_RT_CLOCK);
    }
    midi_clock_phase = (uint8_t)((midi_clock_phase + 1u) & 3u);
}

static void apply_bpm(uint16_t bpm)
{
    /* clockgen_tick_us is the standard 24-PPQN period. Divide by four for
     * the delay engine's 96-PPQN grid, rounding to the nearest microsecond. */
    uint32_t us = (clockgen_tick_us(bpm) + 2u) / 4u;
    struct counter_top_cfg cfg = {
        .ticks = counter_us_to_ticks(clk_ctr, us),
        .callback = tick_top,
        .user_data = NULL,
        /* DONT_RESET: keep the counter running so the new tempo takes effect at the
         * next tick boundary instead of hard-resetting the phase to 0 - a plain
         * reprogram (flags=0) truncates the current period on EVERY BPM change, which
         * makes a fader sweep visibly jump (counter_nrfx_timer.c:283). RESET_WHEN_LATE
         * covers the one case DONT_RESET cannot: when the new (smaller) top is already
         * below the current count, clear now so the tick fires promptly instead of
         * waiting a full 32-bit wrap. The -ETIME it then returns is expected + ignored. */
        .flags = COUNTER_TOP_CFG_DONT_RESET | COUNTER_TOP_CFG_RESET_WHEN_LATE,
    };
    (void)counter_set_top_value(clk_ctr, &cfg);
    cur_bpm = bpm;
}

int clock_timer_init(void)
{
    running = false;
    cur_bpm = 0;
    midi_clock_phase = 0;
    subtick_listener = NULL;
    subtick_listener_ctx = NULL;
    gen_emit = true;
    return device_is_ready(clk_ctr) ? 0 : -1;
}

void clock_timer_set_gen_emit(bool on)
{
    gen_emit = on;
}

bool clock_timer_gen_emit(void)
{
    return gen_emit;
}

void clock_timer_start(uint16_t bpm)
{
    midi_clock_phase = 0;
    apply_bpm(bpm);
    counter_start(clk_ctr);
    running = true;
}

void clock_timer_stop(void)
{
    counter_stop(clk_ctr);
    running = false;
}

void clock_timer_set_bpm(uint16_t bpm)
{
    /* Dedup: a fader sweep fires many CC steps that map to the same BPM; skip the
     * reprogram unless the tempo actually changed, so we do not needlessly clear a
     * pending top event (counter_nrfx_timer.c:277) and drop a tick. */
    if (running && bpm != cur_bpm) {
        apply_bpm(bpm);
    }
}

bool clock_timer_running(void)
{
    return running;
}

void clock_timer_set_subtick_listener(clock_timer_subtick_fn fn, void *ctx)
{
    subtick_listener = fn;
    subtick_listener_ctx = ctx;
}
