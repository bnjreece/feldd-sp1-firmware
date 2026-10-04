#ifndef TRIGGER_OUT_H
#define TRIGGER_OUT_H
#include <stdint.h>
#include <stdbool.h>

/*
 * trigger_out.h — analog trigger / sync pulse output on the TRS jack.
 *
 * The 3.5 mm jack's tip (P0.20) can be EITHER uart1 TX carrying MIDI data — the
 * existing, default behaviour — OR a GPIO emitting analog voltage pulses that
 * advance a step sequencer's trigger input (an SH-01A's EXT CLK IN, a Volca, a
 * Pocket Operator). One conductor, so it is one or the other, chosen by the user.
 *
 * ADDITIVE BY CONSTRUCTION. TRS_MODE_MIDI is 0 and is the default, an absent or
 * out-of-range persisted value decodes to it, and while it is selected this
 * module does not touch the pin, the ring source, or the MIDI path at all. A
 * device that never enables trigger mode behaves exactly as it does today. USB
 * MIDI out and BLE are never affected in either mode — only the TRS jack changes
 * role.
 *
 * ---------------------------------------------------------------------------
 * INTEGRATION — the whole upstream diff, kept deliberately small:
 *
 *  1. midi_out.c   — guard the two TRS enqueue paths with trigger_out_owns_trs()
 *                    so nothing writes UART bytes at a pin we have taken.
 *  2. usb_midi1.c  — call trigger_out_on_voice(vb, vlen) beside the existing
 *                    midi_out_thru(vb, vlen) call; the message is already decoded
 *                    there, so this is one line.
 *  3. librarian.c  — persist the mode in its OWN 1-byte NVS record, exactly as
 *                    midi_thru does (LIB_ID_MIDI_THRU = 3, so take 4). No profile
 *                    version bump: this is a device-level setting, not a per-
 *                    profile one, so the packed profile format is untouched.
 *  4. clock_router — optional, for clock-derived sync: call trigger_out_fire()
 *                    on every Nth tick. Not required by the note-triggered
 *                    feature; see trigger_out_set_divider().
 *
 * Nothing else in the tree needs to know this module exists.
 * ---------------------------------------------------------------------------
 */

/* Role of the TRS jack — one of three, chosen in the configurator and persisted
 * like the MIDI-thru switch. MIDI is 0 = the current behaviour = the default.
 *
 * TRIGGER and SYNC are separate MODES rather than a shared "pulse mode" with a
 * source flag, because that is how the user thinks about them and therefore how
 * the configurator should present them: "what is this jack doing?" They happen to
 * share an output stage, which is an implementation detail. */
enum trs_mode {
	TRS_MODE_MIDI    = 0,   /* uart1 TX, 31250 baud MIDI out (unchanged)     */
	TRS_MODE_TRIGGER = 1,   /* pulse on a matching MIDI note                 */
	TRS_MODE_SYNC    = 2,   /* pulse on a division of the MIDI clock         */
};

/* Persistence helpers, mirroring lib_header.h's midi_thru trio so librarian.c
 * can store this the same way. An absent record or a bad byte decodes to MIDI,
 * so no existing device is reseeded or changes behaviour on upgrade. */
#define TRS_MODE_DEFAULT ((uint8_t)TRS_MODE_MIDI)
static inline int trs_mode_valid(uint8_t v) { return v <= (uint8_t)TRS_MODE_SYNC; }
static inline uint8_t trs_mode_load(int present, uint8_t stored) {
	return (present && trs_mode_valid(stored)) ? stored : TRS_MODE_DEFAULT;
}

/* Persistence helper for the width, mirroring the mode/divider pair. Stored in
 * units of 100 us so it fits one byte: 1..255 = 0.1..25.5 ms. */
#define TRS_WIDTH_UNIT_US   100u
#define TRS_WIDTH_DEFAULT   100u   /* 100 * 100 us = 10 ms */
static inline int trs_width_valid(uint8_t v) { return v >= 1u && v <= 255u; }
static inline uint8_t trs_width_load(int present, uint8_t stored) {
	return (present && trs_width_valid(stored)) ? stored : TRS_WIDTH_DEFAULT;
}

/* Pulse width bounds.
 *
 * DEFAULT 10 ms IS MEASURED. A 3.3 V, 10 ms gate is a known-good combination
 * into an SH-01A's EXT CLK IN, confirmed against a separate 3.3 V sequencer
 * driving the same synth, and confirmed again with this firmware.
 *
 * The upper bound is where the research bites: a TR-909's 20 ms trigger makes
 * this input fire two or three steps from one pulse. So the multi-trigger
 * boundary sits somewhere between 10 ms (works) and 20 ms (does not). Staying at
 * or below 10 ms keeps clear of it, and anything above ~15 ms should be treated
 * as suspect.
 *
 * Resolution: the pulse is timed by TIMER3 at 1 MHz with its falling edge driven
 * by PPI straight into a GPIOTE task, so the width is exact to ~1 us and cannot
 * be stretched by interrupt load. The whole range including the 100 us minimum is
 * therefore usable. (If the GPIOTE channel cannot be allocated the module falls
 * back to k_timer at the system tick's ~30.5 us, where short widths get coarse.) */
#define TRIGGER_WIDTH_US_MIN      100u
#define TRIGGER_WIDTH_US_MAX    50000u
#define TRIGGER_WIDTH_US_DEFAULT 10000u  /* 10 ms — proven; see SPEC §3 Q3 */

/* Wildcard for note/channel matching. */
#define TRIGGER_ANY 0xFFu

/* Default trigger note: 51 — the UPPER-RIGHT pad of a 4x4 bank in an Ableton
 * drum rack or MPC pad bank A (36-39 is the bottom row, so 48-51 is the top).
 * Chosen because it is a corner pad: reachable, memorable, and the least likely
 * square in the bank to already be carrying a drum you care about. In the GM
 * drum map it is Ride Cymbal 1, which is rarely the sound someone is sequencing
 * when they want a trigger lane.
 *
 * Channel defaults to omni so the feature works the moment it is switched on,
 * whatever channel the drum rack happens to send on. Narrow it with
 * trigger_out_set_match() when several sources share the bus. */
#define TRIGGER_NOTE_DEFAULT 51u

/* Match channel, persisted. 0 means OMNI; 1..16 select MIDI channel 1..16 —
 * the channel number as a musician reads it, so the stored byte, the wire and
 * the UI all agree with no off-by-one. (The matcher itself works in 0..15 plus
 * TRIGGER_MATCH_ANY; the conversion lives in trigger_out_set_channel() and
 * nowhere else.) Choosing 0 for OMNI also means an absent or zero-filled record
 * decodes to the intended default rather than silently selecting channel 1.
 * Defaults to OMNI so the feature works the moment it is switched on, but omni
 * is only right when nothing else shares the bus — the moment a drum rack and a
 * synth are both sending, the trigger needs to be told which one to listen to. */
#define TRS_CHAN_OMNI      0u
#define TRS_CHAN_MAX       16u
#define TRS_CHAN_DEFAULT   TRS_CHAN_OMNI
static inline int trs_chan_valid(uint8_t v) { return v <= TRS_CHAN_MAX; }
static inline uint8_t trs_chan_load(int present, uint8_t stored) {
	return (present && trs_chan_valid(stored)) ? stored : TRS_CHAN_DEFAULT;
}

/* Bring up the pulse timer and read the persisted mode. Call once at boot AFTER
 * midi_out_init(), so that in TRS_MODE_MIDI the UART has already claimed the pin
 * and we leave it alone, and in TRS_MODE_TRIGGER we take it back deliberately.
 * Returns 0 on success. A failure leaves the jack in MIDI mode. */
int trigger_out_init(void);

/* Switch the jack's role at runtime. Entering TRIGGER suspends uart1, takes
 * P0.20 as a GPIO parked LOW, and switches the ring current source OFF (a mono
 * TS plug shorts ring to sleeve, and there is no reason to drive a source into
 * that short). Leaving TRIGGER restores both. Persisting the choice is the
 * caller's job — see librarian_set_midi_thru() for the pattern. */
int trigger_out_set_mode(enum trs_mode m);
enum trs_mode trigger_out_mode(void);

/* Which pulse path came up: true = TIMER3+PPI+GPIOTE (edges in hardware), false =
 * k_timer fallback (30.5 us resolution, and the width can be stretched by
 * interrupt load). Exposed so the device can REPORT it — the boot printk is easy
 * to miss, and a silent downgrade would otherwise only show up as mysteriously
 * sloppy timing. */
bool trigger_out_hw_pulse(void);

/* Diagnostics that stand in for a multimeter.
 *
 * fire_count: how many pulses have actually been ASSERTED. Non-zero proves the
 * note match / clock divider reached the output stage, so a dead jack with a
 * rising count is an electrical problem, not a routing one.
 *
 * busy: 1 while a pulse is high. If this STAYS 1, the falling edge never
 * happened — TIMER3 or the PPI connection is not working and the pin is latched
 * high. That is the same fact a meter reading a steady 3.3 V would tell you. */
uint32_t trigger_out_fire_count(void);
bool     trigger_out_busy(void);

/* The pin's ACTUAL idle level, read back from the GPIO OUT latch. In trigger
 * mode this must be 0 between pulses: a trigger input that gates for as long as
 * the line is high turns an idle-high pin into a permanently sustained note.
 * Reported rather than assumed: a value the firmware merely intends is not
 * evidence of what the pad is doing. */
bool trigger_out_pin_high(void);

/* True when entering a pulse mode FAILED to win the pad — the pin was driven
 * both ways and did not follow. Reported so a failed handoff is visible instead
 * of being silently claimed as success. */
bool trigger_out_pin_fault(void);

/* Polarity. 0 (default) = idle LOW, pulse HIGH — ordinary V-trig. 1 = idle HIGH,
 * pulse LOW, for an input that wants a pull-down, or for a tip that can only
 * SINK because the MIDI Type A current source lives on the ring. Worth trying
 * when a V-trig pulse the firmware believes it is emitting does not move the
 * receiving device. */
/* The ring current source while in a pulse mode. Type A MIDI puts the source on
 * the ring and switches it with the tip, so the source being ON is very likely
 * required for the tip to drive anything at all. Defaults ON, matching MIDI
 * mode — the configuration known to deliver a usable signal on this jack. */
bool trigger_out_ring_on(void);
int  trigger_out_set_ring(bool on);

bool trigger_out_invert(void);
int  trigger_out_set_invert(bool inv);

/* True while this module owns P0.20. midi_out.c consults this before enqueuing
 * TRS bytes; USB and BLE sinks ignore it and keep working. */
bool trigger_out_owns_trs(void);

/* Emit one pulse now. Safe from ISR or thread context. A no-op in MIDI mode, and
 * a no-op while a pulse is already high — a chord that matches several notes
 * fires ONE step, not one per note, which is what a trigger should do. */
void trigger_out_fire(void);

/* Feed a decoded channel-voice message (status, d1[, d2]) from the USB-MIDI OUT
 * endpoint. Fires when it matches the configured note + channel. No-op in MIDI
 * mode. `len` is 2 or 3, as produced by usb_midi_extract_voice(). */
void trigger_out_on_voice(const uint8_t *bytes, uint8_t len);

/* Pulse width, microseconds; clamped to [TRIGGER_WIDTH_US_MIN, _MAX]. Runtime
 * settable so the width can be dialled in against real gear instead of guessed —
 * the correct value is a property of the receiving input, not of this firmware. */
int      trigger_out_set_width_us(uint32_t us);
uint32_t trigger_out_width_us(void);

/* Which note fires the trigger. Either may be TRIGGER_ANY (any note / omni). */
int trigger_out_set_match(uint8_t note, uint8_t channel);

/* Channel alone, leaving the note as it is. `ch` is TRS_CHAN_OMNI (0) for omni,
 * or 1..16 for MIDI channel 1..16. */
int     trigger_out_set_channel(uint8_t ch);
uint8_t trigger_out_channel(void);

/* SYNC-mode rate: fire every Nth MIDI clock tick. MIDI clock is 24 PPQN, so
 * 12 = 2 PPQN = the Pocket Operator / Volca rate (the default), 6 = one pulse per
 * 16th, 24 = one per quarter. Persisted alongside the mode. Range 1..24.
 *
 * trigger_out_clock_tick() is called from midi_out_rt() on every 0xF8, which is
 * the one point BOTH clock sources funnel through — the internal generator
 * (clock_timer's TIMER2 ISR) and an external master (clock_router_ext_rt from the
 * USB OUT callback). feldd's "never two clocks" tenet guarantees only one is live,
 * so sync out follows whichever owns the clock with no extra coordination. */
#define TRIGGER_DIV_MIN      1u
#define TRIGGER_DIV_MAX     24u
#define TRIGGER_DIV_DEFAULT 12u   /* 2 PPQN — the PO / Volca rate */
static inline int trs_div_valid(uint8_t v) {
	return v >= TRIGGER_DIV_MIN && v <= TRIGGER_DIV_MAX;
}
static inline uint8_t trs_div_load(int present, uint8_t stored) {
	return (present && trs_div_valid(stored)) ? stored : TRIGGER_DIV_DEFAULT;
}
int      trigger_out_set_divider(uint8_t n);
uint8_t  trigger_out_divider(void);
void     trigger_out_clock_tick(void);

#endif /* TRIGGER_OUT_H */
