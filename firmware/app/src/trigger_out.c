/*
 * trigger_out.c — analog trigger / sync pulse output on the TRS jack.
 *
 * A deliberately dumb output stage: "assert the tip for N microseconds, now."
 * It knows nothing about music. Two callers feed it — a note match
 * (trigger_out_on_voice) and a MIDI-clock divider (trigger_out_clock_tick) —
 * and keeping it ignorant of which is what makes the second nearly free.
 *
 * The whole feature is additive. TRS_MODE_MIDI is the default and, while it is
 * selected, every function here is an early return: the pin, the ring current
 * source, and midi_out's UART path are untouched. Enabling trigger mode changes
 * the role of ONE jack and nothing else — USB MIDI and BLE are unaffected in
 * both modes.
 *
 * THE PIN HANDOFF is the delicate part. P0.20 is uart1 TX (app.overlay:32), so
 * MIDI out and trigger out cannot coexist on it — a mono TS patch cable takes
 * its signal from the tip and grounds everything else, so there is no second
 * conductor to move to. Entering trigger mode therefore suspends the UART and
 * takes the pin as GPIO; leaving restores both. See trs_take_pin/trs_release_pin.
 *
 * IDLE STATE MATTERS. UART TX idles HIGH (mark). Left that way, a sequencer's
 * trigger input sees a permanently asserted level and our "pulse" would be a
 * downward dip from it. So the pin is parked LOW before it is ever driven, and
 * pulses go HIGH — ordinary V-trig.
 *
 * Pulse width and polarity are runtime-settable because the right values are a
 * property of the receiving input, not of this firmware.
 */
#include "trigger_out.h"
#include "trigger_match.h"

#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/devicetree.h>
#include <zephyr/pm/device.h>
#include <hal/nrf_gpio.h>
#include <hal/nrf_uarte.h>
#include <hal/nrf_timer.h>
#include <hal/nrf_gpiote.h>
#include <nrfx_gpiote.h>
#include <helpers/nrfx_gppi.h>
#include <errno.h>

/* Tip: MIDI data in TRS_MODE_MIDI, our pulse output in TRS_MODE_TRIGGER. */
#define TRIG_PIN NRF_GPIO_PIN_MAP(0, 20)

static const struct device *const trs_uart = DEVICE_DT_GET(DT_NODELABEL(uart1));

/* Our OWN DT-derived pinctrl config for uart1. This looks like it duplicates the
 * UART driver's, and it does — but the driver's is static to its translation
 * unit, so PINCTRL_DT_DEV_CONFIG_GET alone fails to link
 * ("__pinctrl_dev_config__device_dts_ord_NN undeclared"). Both copies describe
 * the same DT node, and applying a state writes the same PSEL registers, so this
 * is a few bytes of duplicated const flash rather than a second source of truth. */
PINCTRL_DT_DEFINE(DT_NODELABEL(uart1));
static const struct pinctrl_dev_config *const trs_pcfg =
	PINCTRL_DT_DEV_CONFIG_GET(DT_NODELABEL(uart1));

/* Ring: the PNP current-source base, ACTIVE-LOW (app.overlay:11-14). midi_out.c
 * owns this in MIDI mode; we take our own handle rather than reaching into that
 * module, and the two never drive it at once because the mode is exclusive. */
static const struct gpio_dt_spec ring =
	GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), midi_ring_gpios);

static enum trs_mode g_mode = TRS_MODE_MIDI;
static uint32_t g_width_us = TRIGGER_WIDTH_US_DEFAULT;
static struct trigger_match_cfg g_match = {
	.note = TRIGGER_NOTE_DEFAULT,      /* 51: top-right pad of a 4x4 bank */
	.channel = TRIGGER_MATCH_ANY,      /* omni until the user narrows it   */
};
static uint8_t g_divider = TRIGGER_DIV_DEFAULT;   /* SYNC rate: ticks per pulse */
static uint8_t g_tick_count;

static bool g_pin_fault;               /* set when the pad will not follow us */
static bool g_ring_on = true;          /* ring current source in pulse modes */
static bool g_invert;                  /* 1 = idle HIGH, pulse LOW (sink-style) */
static uint32_t g_fire_count;          /* pulses asserted; a meterless witness */
static struct k_timer g_pulse_timer;   /* fallback path only; see below */
static atomic_t g_pulse_active;        /* 1 while the tip is HIGH */

/* ---------------------------------------------------------------------------
 * HARDWARE PULSE PATH — TIMER3 + PPI + GPIOTE.
 *
 * The falling edge is generated ENTIRELY IN HARDWARE: TIMER3's COMPARE0 event is
 * wired by PPI straight to a GPIOTE task that clears the pin. No ISR is involved
 * in ending the pulse, so its width is exact to the timer clock (1 us here) and
 * cannot be stretched by interrupt load — a fader SAADC burst, a USB transfer or
 * an LED PWM update can no longer lengthen a trigger.
 *
 * This mirrors a decision feldd already made once: clock_timer.c moved the MIDI
 * clock off k_timer onto hardware TIMER2 because "k_timer is quantised to the
 * 32768 Hz system tick [which] jittered the tempo above ~100 BPM"
 * (clock_timer.c:4-8). Same reasoning, one step further — that path still ends in
 * an ISR, this one does not.
 *
 * TIMER3 is chosen because timer0/1/3/4 are all DT-disabled in this build (only
 * timer2 is claimed, by the clock generator), so no Zephyr driver owns it and raw
 * HAL access is safe. Driving peripherals by register is already house style here
 * (main.c does it for NRF_WDT and nrf_gpio).
 *
 * The k_timer path is kept as a fallback for when the GPIOTE channel cannot be
 * allocated. It is functionally identical at 30.5 us resolution.
 * ------------------------------------------------------------------------- */
#define TRIG_TIMER      NRF_TIMER3
#define TRIG_TIMER_HZ   1000000u   /* 1 MHz: 1 us resolution, 32-bit counter */

/* PPI connection, ALLOCATED not hardcoded.
 *
 * An earlier version pinned channel 19 on the assumption that nothing else in
 * this build used PPI. That was wrong: Zephyr's UARTE driver allocates a channel
 * of its own to wire ENDTX -> TXSTOP (UARTE_CFG_FLAG_PPI_ENDTX,
 * uart_nrfx_uarte.c:286). Pinning 19 overwrote whatever endpoints it had been
 * given, so TRS MIDI TX stopped working while everything else looked fine.
 *
 * nrfx_gppi_conn_alloc is the same API the UARTE driver uses, so both go through
 * one allocator and cannot collide. Hardcoding a shared hardware resource in a
 * tree you do not fully control is the mistake here — not the specific number. */
static nrfx_gppi_handle_t g_ppi;
static uint8_t g_gpiote_ch;
static bool    g_hw_ready;     /* false = fall back to k_timer */

/* ------------------------------------------------------------------------- */

/* FALLBACK path only (g_hw_ready == false): drop the tip from the system-clock
 * ISR. Correct, but the width carries the tick's 30.5 us quantisation and can be
 * stretched by interrupt load. */
static void pulse_end(struct k_timer *t)
{
	ARG_UNUSED(t);
	nrf_gpio_pin_clear(TRIG_PIN);
	atomic_clear(&g_pulse_active);
}

/* TIMER3 ISR. The pin is ALREADY LOW by the time this runs — PPI cleared it in
 * hardware at the compare event. This only releases the coalescing flag, which is
 * bookkeeping and may be late without affecting the pulse. That split is the
 * whole point: timing-critical work in hardware, housekeeping in software. */
static void trig_timer_isr(const void *arg)
{
	ARG_UNUSED(arg);
	if (nrf_timer_event_check(TRIG_TIMER, NRF_TIMER_EVENT_COMPARE0)) {
		nrf_timer_event_clear(TRIG_TIMER, NRF_TIMER_EVENT_COMPARE0);

		/* BACKSTOP FALLING EDGE. In principle PPI has already cleared the pin
		 * at the compare event and this is a no-op — the CLR task is
		 * idempotent. In practice the PPI route silently did NOT fire on
		 * hardware: the pin latched high, and because this ISR clears the busy
		 * flag whether or not the pad actually moved, everything downstream
		 * reported healthy while an SH-01A sat holding one note forever.
		 *
		 * A stuck-high trigger output is the worst failure this module can
		 * have — it is a sustained gate into whatever is patched in — so the
		 * falling edge must not depend on a single mechanism working. Costs one
		 * register write in an ISR that already runs. */
		if (g_hw_ready) {
			nrf_gpiote_task_trigger(NRF_GPIOTE,
						g_invert ? nrf_gpiote_set_task_get(g_gpiote_ch)
							 : nrf_gpiote_clr_task_get(g_gpiote_ch));
		}
		/* belt and braces if GPIOTE is not driving the pad */
		if (g_invert) { nrf_gpio_pin_set(TRIG_PIN); }
		else          { nrf_gpio_pin_clear(TRIG_PIN); }

		atomic_clear(&g_pulse_active);
	}
}

/* Wire TIMER3 -> PPI -> GPIOTE so the falling edge needs no software at all.
 * Returns true if the hardware path is usable. */
static bool hw_pulse_init(void)
{
#ifdef TRIGGER_NO_HW_PULSE
	/* Build-time escape hatch: -DTRIGGER_NO_HW_PULSE compiles out ALL of the
	 * TIMER + PPI + GPIOTE path, leaving the k_timer fallback. Useful for
	 * isolating a fault to the hardware pulse path, and as a fallback for a
	 * target where those peripherals are unavailable. */
	return false;
#else
	/* NRFX_GPIOTE_INSTANCE takes the REGISTER BASE, not an instance index —
	 * `#define NRFX_GPIOTE_INSTANCE(reg)` sets .p_reg = (NRF_GPIOTE_Type *)reg
	 * and derives .cb.drv_inst_idx via NRFX_REG_TO_INSTANCE(GPIOTE, reg).
	 *
	 * Passing 0 (as an earlier version did) yields p_reg = NULL and a garbage
	 * drv_inst_idx, and nrfx_gpiote_channel_alloc then indexes its internal
	 * allocator array with that garbage and writes OUT OF BOUNDS into unrelated
	 * RAM. It still returned 0, so the failure was silent — it corrupted the
	 * uart1 driver's state and killed TRS MIDI out while everything else kept
	 * working. Confirmed on hardware. */
	nrfx_gpiote_t gpiote = NRFX_GPIOTE_INSTANCE(NRF_GPIOTE);

	if (nrfx_gpiote_channel_alloc(&gpiote, &g_gpiote_ch) != 0) {
		return false;   /* caller falls back to k_timer */
	}

	/* Bound-check what we were handed. The nRF52840 has 8 GPIOTE channels; a
	 * value outside that means the allocator misbehaved and every task address
	 * we derive from it would point somewhere arbitrary. Refuse rather than
	 * proceed — the k_timer path is correct, just less precise. */
	if (g_gpiote_ch >= 8u) {
		return false;
	}

	/* NOTE: the GPIOTE channel is allocated here but deliberately NOT configured.
	 * On the nRF52840, writing CONFIG[n].MODE = Task hands the pin to GPIOTE
	 * IMMEDIATELY — there is no separate arm step — so configuring it at boot
	 * would steal P0.20 from uart1 even in MIDI mode and silently kill TRS MIDI
	 * out. Configuration happens in trs_take_pin(), on the MIDI -> pulse
	 * transition, and is undone in trs_release_pin(). */

	/* 1 MHz, 32-bit, one-shot: CLEAR+STOP on compare so it re-arms itself and
	 * never free-runs. */
	nrf_timer_mode_set(TRIG_TIMER, NRF_TIMER_MODE_TIMER);
	nrf_timer_bit_width_set(TRIG_TIMER, NRF_TIMER_BIT_WIDTH_32);
	/* 16 MHz base / 2^4 = 1 MHz. nrf_timer_frequency_set() was removed in
	 * nrfx 3.x; the prescaler is now set directly. */
	nrf_timer_prescaler_set(TRIG_TIMER, 4);
	nrf_timer_shorts_enable(TRIG_TIMER,
				NRF_TIMER_SHORT_COMPARE0_CLEAR_MASK |
				NRF_TIMER_SHORT_COMPARE0_STOP_MASK);
	nrf_timer_int_enable(TRIG_TIMER, NRF_TIMER_INT_COMPARE0_MASK);

	IRQ_CONNECT(TIMER3_IRQn, 1, trig_timer_isr, NULL, 0);
	irq_enable(TIMER3_IRQn);

	/* THE point of all this: compare event -> clear the pin, in silicon. */
	if (nrfx_gppi_conn_alloc(
		nrf_timer_event_address_get(TRIG_TIMER, NRF_TIMER_EVENT_COMPARE0),
		nrf_gpiote_task_address_get(NRF_GPIOTE,
			nrf_gpiote_clr_task_get(g_gpiote_ch)),
		&g_ppi) != 0) {
		nrfx_gpiote_channel_free(&gpiote, g_gpiote_ch);
		return false;   /* no PPI to be had: fall back to k_timer */
	}
	nrfx_gppi_conn_enable(g_ppi);

	return true;
#endif /* TRIGGER_NO_HW_PULSE */
}

/* Take P0.20 from uart1.
 *
 * The pinctrl SLEEP state is what actually releases the pad: app.overlay's
 * uart1_sleep carries `low-power-enable`, so applying it disconnects the UARTE's
 * PSEL.TXD from P0.20 and leaves the pin free for GPIO. feldd does NOT enable
 * CONFIG_PM_DEVICE, so the pm_device_action_run() below compiles out entirely in
 * the shipping config — it is kept for builds that do enable PM, where suspending
 * the device first is tidier. Either way the UART has no bytes in flight, because
 * midi_out stops feeding it the moment trigger_out_owns_trs() goes true.
 *
 * VERIFY ON HARDWARE: that the sleep state fully detaches the pad, and that a
 * later DEFAULT re-apply restores MIDI out, are the two things in this module
 * most likely to need iteration. Both are observable — scope the tip, or just
 * send MIDI after switching back. */
static void trs_take_pin(void)
{
#if defined(CONFIG_PM_DEVICE)
	(void)pm_device_action_run(trs_uart, PM_DEVICE_ACTION_SUSPEND);
#endif
	(void)pinctrl_apply_state(trs_pcfg, PINCTRL_STATE_SLEEP);

	/* THE ACTUAL RELEASE. Neither of the above detaches the peripheral: feldd
	 * does not enable CONFIG_PM_DEVICE, so the suspend compiles out entirely,
	 * and applying the pinctrl sleep state did NOT stop the UARTE driving the
	 * pad. Measured on hardware: the firmware reported pin=0 while a scope
	 * showed the line sitting at 3.3 V — which is exactly what an enabled UARTE
	 * holds on TXD when idle (MIDI mark).
	 *
	 * That single fact explains the whole evening. The UART was holding the line
	 * high the entire time we were in trigger mode: hence the permanently gated
	 * note on the SH-01A, hence pulses that never produced a clean edge because
	 * the GPIO was fighting a peripheral for the pad, and hence a self-test that
	 * reported everything healthy while nothing reached the synth.
	 *
	 * Disconnecting PSEL.TXD is what actually makes the UARTE let go. Kept
	 * separate from ENABLE so the driver's own state is untouched — with the
	 * pin unrouted it simply transmits into nowhere, and midi_out is gated off
	 * anyway while we own the jack. */
	nrf_uarte_tx_pin_set(NRF_UARTE1, NRF_UARTE_PSEL_DISCONNECTED);

	/* Park LOW *before* becoming an output, so the pin never presents a
	 * spurious asserted level to whatever is plugged in.
	 *
	 * HIGH DRIVE (H0H1), not nrf_gpio_cfg_output's standard S0S1. The tip
	 * reaches the jack through the MIDI Type A output network, and standard
	 * drive (~2 mA) into that series resistance plus cable capacitance may not
	 * swing far or fast enough for a trigger input to see a clean edge —
	 * consistent with a pulse that sometimes triggers, sometimes double-triggers
	 * and sometimes does nothing, while the stock firmware's PO-sync drives the
	 * same synth reliably. H0H1 gives ~5 mA and a faster edge. */
	nrf_gpio_pin_clear(TRIG_PIN);
	nrf_gpio_cfg(TRIG_PIN,
		     NRF_GPIO_PIN_DIR_OUTPUT,
		     NRF_GPIO_PIN_INPUT_CONNECT,   /* so the pad can be READ BACK */
		     NRF_GPIO_PIN_NOPULL,
		     NRF_GPIO_PIN_H0H1,
		     NRF_GPIO_PIN_NOSENSE);
	nrf_gpio_pin_clear(TRIG_PIN);

	/* PROVE THE HANDOFF. Drive the pad both ways and read the pad back. If it
	 * does not follow, something else still owns it — which is what happens if the
	 * UARTE keeps driving TXD while every software indicator says otherwise.
	 * A mode change that did not actually occur must not be
	 * silently claimed.
	 *
	 * THIS MUST RUN BEFORE GPIOTE TAKES THE PIN. In Task mode GPIOTE overrides
	 * the GPIO OUT register, so nrf_gpio_pin_set() below would do nothing, the
	 * pad would sit at GPIOTE's initial LOW value, saw_high would read 0, and
	 * the check would report a fault on EVERY successful entry. Confirmed on
	 * hardware: fault=1 with the hardware pulse path enabled and fault=0 with it
	 * compiled out, while pulses fired correctly either way. A diagnostic that
	 * cries wolf is worse than none, because it masks the real fault it exists
	 * to catch.
	 *
	 * Proving here is also sufficient: what needs verifying is that the UARTE
	 * released the pad, which has already happened by this point. GPIOTE taking
	 * over afterwards is deterministic and needs no proof. */
	nrf_gpio_pin_set(TRIG_PIN);
	k_busy_wait(5);
	bool saw_high = nrf_gpio_pin_read(TRIG_PIN) != 0;
	nrf_gpio_pin_clear(TRIG_PIN);
	k_busy_wait(5);
	bool saw_low = nrf_gpio_pin_read(TRIG_PIN) == 0;
	g_pin_fault = !(saw_high && saw_low);
	if (g_pin_fault) {
		printk("TRIG PIN FAULT: pad will not follow (high=%d low=%d) — "
		       "something else still owns P0.20\n", saw_high, saw_low);
	}

	/* Hand the pin to GPIOTE so PPI can drive both edges. Done AFTER the GPIO
	 * config above so the pad direction and its LOW level are already settled
	 * when GPIOTE takes over. MODE = Task claims the pin as a side effect, which
	 * is exactly what we want HERE and precisely why it must not happen at boot. */
	if (g_hw_ready) {
		nrf_gpiote_task_configure(NRF_GPIOTE, g_gpiote_ch, TRIG_PIN,
					  NRF_GPIOTE_POLARITY_NONE,
					  g_invert ? NRF_GPIOTE_INITIAL_VALUE_HIGH
						   : NRF_GPIOTE_INITIAL_VALUE_LOW);
		nrf_gpiote_task_enable(NRF_GPIOTE, g_gpiote_ch);
		/* Explicitly drive it low once GPIOTE owns the pad. OUTINIT should have
		 * done this, but an idle-HIGH trigger output is a sustained gate into
		 * the patched synth, so it is worth one redundant write. */
		nrf_gpiote_task_trigger(NRF_GPIOTE,
					g_invert ? nrf_gpiote_set_task_get(g_gpiote_ch)
						 : nrf_gpiote_clr_task_get(g_gpiote_ch));
	}


	/* Ring current source LEFT ON by default.
	 *
	 * An earlier version switched it OFF here, on my own reasoning that a mono
	 * TS plug shorts ring to sleeve so there was "no reason to drive a source
	 * into a short". That reasoning was invented, not verified, and it is very
	 * likely why nothing worked: in a Type A MIDI OUT the RING IS THE CURRENT
	 * SOURCE and the tip merely switches it. Kill the source and the tip has
	 * nothing to drive, which fits every observation — MIDI works in mode 0
	 * (source on), triggers never worked in mode 1 (source off), and the stock
	 * firmware's PO-sync drives the same synth through the same TS cable.
	 *
	 * Configurable via trsring for the case where it genuinely should be off,
	 * but the default now matches what MIDI mode does, because that is the
	 * configuration known to put a usable signal on this jack. */
	if (gpio_is_ready_dt(&ring)) {
		gpio_pin_set_dt(&ring, g_ring_on ? 1 : 0);
	}
}

/* Give P0.20 back to uart1 and restore the MIDI electrical path. */
static void trs_release_pin(void)
{
	/* Release GPIOTE's claim first, or the pad stays under its control and
	 * pinctrl's restore of uart1 TX would not take effect. task_disable clears
	 * MODE back to Disabled, which is what actually returns the pad. */
	if (g_hw_ready) {
		nrf_gpiote_task_disable(NRF_GPIOTE, g_gpiote_ch);
	}
	nrf_gpio_pin_clear(TRIG_PIN);
	nrf_gpio_cfg_default(TRIG_PIN);          /* stop driving before pinctrl */
	(void)pinctrl_apply_state(trs_pcfg, PINCTRL_STATE_DEFAULT);
	/* Re-route the UARTE's TX back to the pad. The mirror of the disconnect in
	 * trs_take_pin; without this, MIDI out would come back silent. */
	nrf_uarte_tx_pin_set(NRF_UARTE1, TRIG_PIN);
#if defined(CONFIG_PM_DEVICE)
	(void)pm_device_action_run(trs_uart, PM_DEVICE_ACTION_RESUME);
#endif
	if (gpio_is_ready_dt(&ring)) {
		gpio_pin_set_dt(&ring, 1);           /* logical 1 = source on */
	}
}

/* ------------------------------------------------------------------------- */

int trigger_out_init(void)
{
	k_timer_init(&g_pulse_timer, pulse_end, NULL);
	atomic_clear(&g_pulse_active);

	/* Prefer the hardware path; degrade to k_timer rather than failing if the
	 * GPIOTE channel cannot be had. Both produce a correct pulse — only the
	 * width's precision and its immunity to interrupt load differ. */
	g_hw_ready = hw_pulse_init();
	printk("TRIG pulse path: %s\n", g_hw_ready ? "hardware (TIMER3+PPI+GPIOTE)"
						   : "software (k_timer fallback)");

	/* Deliberately does NOT touch the pin. At boot the mode is MIDI until a
	 * caller (librarian-restored setting, config protocol, gesture) says
	 * otherwise, and MIDI mode must be bit-for-bit the existing behaviour. */
	g_mode = TRS_MODE_MIDI;
	return device_is_ready(trs_uart) ? 0 : -ENODEV;
}

enum trs_mode trigger_out_mode(void) { return g_mode; }

/* True in BOTH pulse modes: either way the UART no longer owns the pin. */
bool trigger_out_owns_trs(void) { return g_mode != TRS_MODE_MIDI; }

bool trigger_out_hw_pulse(void) { return g_hw_ready; }

uint32_t trigger_out_fire_count(void) { return g_fire_count; }

bool trigger_out_pin_fault(void) { return g_pin_fault; }

bool trigger_out_ring_on(void) { return g_ring_on; }

int trigger_out_set_ring(bool on)
{
	g_ring_on = on;
	if (g_mode != TRS_MODE_MIDI && gpio_is_ready_dt(&ring)) {
		gpio_pin_set_dt(&ring, on ? 1 : 0);
	}
	return 0;
}

bool trigger_out_invert(void) { return g_invert; }

int trigger_out_set_invert(bool inv)
{
	if (inv == g_invert) {
		return 0;
	}
	g_invert = inv;
	/* Re-park the pin at the new idle level if we already own it. */
	if (g_mode != TRS_MODE_MIDI) {
		if (g_hw_ready) {
			nrf_gpiote_task_trigger(NRF_GPIOTE,
						g_invert ? nrf_gpiote_set_task_get(g_gpiote_ch)
							 : nrf_gpiote_clr_task_get(g_gpiote_ch));
		}
		if (g_invert) { nrf_gpio_pin_set(TRIG_PIN); }
		else          { nrf_gpio_pin_clear(TRIG_PIN); }
	}
	return 0;
}
/* Reads the ACTUAL PAD via the input buffer, not the OUT latch.
 *
 * This used nrf_gpio_pin_out_read, which returns what we ASKED for. It therefore
 * reported "low" for hours while a scope showed the pad held at 3.3 V by a UARTE
 * we had failed to detach — a readback that can only ever confirm its own
 * intent, which is worse than none because it ends the search. */
bool trigger_out_pin_high(void) { return nrf_gpio_pin_read(TRIG_PIN) != 0; }
bool     trigger_out_busy(void) { return atomic_get(&g_pulse_active) != 0; }

int trigger_out_set_mode(enum trs_mode m)
{
	if (!trs_mode_valid((uint8_t)m)) {
		return -EINVAL;
	}
	if (m == g_mode) {
		return 0;
	}

	if (m != TRS_MODE_MIDI) {
		/* Flip the flag FIRST. midi_out consults trigger_out_owns_trs()
		 * before enqueuing, so this closes the UART's tap before we pull the
		 * pin out from under it, rather than after. TRIGGER <-> SYNC needs no
		 * hardware work — we already own the pin — so only the MIDI boundary
		 * touches pinctrl. */
		bool had_pin = (g_mode != TRS_MODE_MIDI);
		g_mode = m;
		if (!had_pin) {
			trs_take_pin();
		}
	} else {
		/* Cancel any pulse in flight so we never restore the UART with the
		 * timer still due to write the pin behind its back. */
		k_timer_stop(&g_pulse_timer);
		if (g_hw_ready) {
			nrf_timer_task_trigger(TRIG_TIMER, NRF_TIMER_TASK_STOP);
		}
		atomic_clear(&g_pulse_active);
		trs_release_pin();
		g_mode = TRS_MODE_MIDI;
	}
	return 0;
}

int trigger_out_set_width_us(uint32_t us)
{
	if (us < TRIGGER_WIDTH_US_MIN || us > TRIGGER_WIDTH_US_MAX) {
		return -EINVAL;
	}
	g_width_us = us;
	return 0;
}

uint32_t trigger_out_width_us(void) { return g_width_us; }

int trigger_out_set_match(uint8_t note, uint8_t channel)
{
	struct trigger_match_cfg c = { .note = note, .channel = channel };

	if (!trigger_match_cfg_valid(&c)) {
		return -EINVAL;
	}
	g_match = c;
	return 0;
}

int trigger_out_set_channel(uint8_t ch)
{
	if (!trs_chan_valid(ch)) {
		return -EINVAL;
	}
	/* Stored/wire form is 0 = omni, 1..16 = channel, matching what the UI shows.
	 * The matcher speaks 0..15 plus TRIGGER_MATCH_ANY, so the two representations
	 * are reconciled HERE and nowhere else — keep it that way. */
	g_match.channel = (ch == TRS_CHAN_OMNI)
	                      ? TRIGGER_MATCH_ANY : (uint8_t)(ch - 1u);
	return 0;
}

uint8_t trigger_out_channel(void)
{
	return (g_match.channel == TRIGGER_MATCH_ANY)
	           ? TRS_CHAN_OMNI : (uint8_t)(g_match.channel + 1u);
}

int trigger_out_set_divider(uint8_t n)
{
	if (!trs_div_valid(n)) {
		return -EINVAL;
	}
	g_divider = n;
	g_tick_count = 0;
	return 0;
}

uint8_t trigger_out_divider(void) { return g_divider; }

void trigger_out_fire(void)
{
	if (g_mode == TRS_MODE_MIDI) {
		return;
	}

	/* Coalesce. A chord whose notes all match, or two events closer together
	 * than the pulse width, must produce ONE step — not one per note. Winning
	 * this CAS is what grants the right to drive the pin, so the timer callback
	 * can never race a second assertion. */
	if (!atomic_cas(&g_pulse_active, 0, 1)) {
		return;
	}
	g_fire_count++;

	if (g_hw_ready) {
		/* Arm the width, then raise the pin. Both are single register writes,
		 * so onset is as immediate as the caller's context allows; the falling
		 * edge is then hardware's problem alone. */
		nrf_timer_cc_set(TRIG_TIMER, NRF_TIMER_CC_CHANNEL0, g_width_us);
		nrf_timer_task_trigger(TRIG_TIMER, NRF_TIMER_TASK_CLEAR);
		nrf_gpiote_task_trigger(NRF_GPIOTE,
					g_invert ? nrf_gpiote_clr_task_get(g_gpiote_ch)
						 : nrf_gpiote_set_task_get(g_gpiote_ch));
		nrf_timer_task_trigger(TRIG_TIMER, NRF_TIMER_TASK_START);
	} else {
		if (g_invert) { nrf_gpio_pin_clear(TRIG_PIN); }
		else          { nrf_gpio_pin_set(TRIG_PIN); }
		k_timer_start(&g_pulse_timer, K_USEC(g_width_us), K_NO_WAIT);
	}
}

void trigger_out_on_voice(const uint8_t *bytes, uint8_t len)
{
	if (g_mode != TRS_MODE_TRIGGER) {
		return;   /* MIDI mode, or SYNC mode where the clock owns the output */
	}
	if (trigger_match(&g_match, bytes, len)) {
		trigger_out_fire();
	}
}

void trigger_out_clock_tick(void)
{
	if (g_mode != TRS_MODE_SYNC) {
		return;   /* MIDI mode, or TRIGGER mode where notes own the output */
	}
	if (++g_tick_count >= g_divider) {
		g_tick_count = 0;
		trigger_out_fire();
	}
}
