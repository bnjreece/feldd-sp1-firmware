#ifndef SP1_CLOCK_TIMER_H
#define SP1_CLOCK_TIMER_H
#include <stdint.h>
#include <stdbool.h>

/*
 * clock_timer.h - the internal 24-PPQN MIDI clock generator (GEN mode).
 *
 * A dedicated nRF hardware TIMER (timer2 @ 1 MHz via the Zephyr counter API) fires a
 * periodic top-value callback in its own ISR that emits a MIDI clock byte (0xF8) via
 * midi_out_rt(), which drops it in the TRS priority tier. The hardware timer gives
 * 1 us resolution - vs the ~30 us of a k_timer bound to the 32768 Hz system tick,
 * which jittered the tempo visibly above ~100 BPM. Combined with the priority TX that
 * bounds delay to one in-flight byte (~320 us), tick jitter stays well inside the
 * feature's target. Tempo is set in BPM; the period is clockgen_tick_us(bpm). This is
 * the source when no external clock is present.
 */
int  clock_timer_init(void);            /* one-time hardware-timer init */
void clock_timer_start(uint16_t bpm);   /* begin ticking at bpm */
void clock_timer_stop(void);            /* stop ticking */
void clock_timer_set_bpm(uint16_t bpm); /* change tempo live (no-op if not running) */
bool clock_timer_running(void);

/* GEN-emit gate: while an external master is passed through (THRU), the free-running
 * generator must stay silent so the two clocks never overlap on the wire. The timer
 * keeps running; only its ISR emit is gated (avoids a start/stop race on mode flips).
 * Defaults to on (GEN). */
void clock_timer_set_gen_emit(bool on);
bool clock_timer_gen_emit(void);

#endif /* SP1_CLOCK_TIMER_H */
