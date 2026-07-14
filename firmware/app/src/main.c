/*
 * SP-1 controller firmware — main.
 *
 * Boot contract (the SP-1 "BIG FIVE"): app lives at 0x20000; watchdog fed
 * < 5 s; we do NOT re-init bootloader-owned clocks/peripherals; SYSTEM_OFF on a
 * •• hold returns to the bootloader; RESETREAS is cleared on boot and before
 * SYSTEM_OFF. (There is no hardware reset pin on the SP-1, so a clean path back
 * to the bootloader is mandatory.)
 *
 * M2.2 adds: SAADC read of the 4 faders + 2 button ladders + battery
 * (controls.c) and a USB-CDC console (device_next stack via the sample_usbd
 * helper).
 * M4.1 adds: USB-MIDI 1.0 (hand-rolled usb_midi1.c class) to the same composite
 * as the CDC console (both ride one usbd context, brought up by usbdev_start),
 * and a USB sink in midi_out so the mapping engine emits to BOTH the TRS UART
 * and USB-MIDI.
 * M6.2 adds: the CDC console is now the JSON-lines config protocol channel
 * (config_cdc.c wraps the librarian as a proto_store for protocol.c) plus a
 * live monitor stream of fader CCs / button edges. The old per-loop raw-ADC and
 * per-event MIDI/BTN debug printk chatter is removed so the port is a clean
 * structured stream for the web config tool.
 * HARDWARE VERIFICATION DEFERRED: no SAADC/USBD model in Renode and Unit A is
 * not available, so this is BUILD-VERIFIED only — the live fader read, the TRS
 * electrical bench-test, and USB-MIDI computer-enumeration are checked when
 * Unit A is on the bench.
 */
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/fatal.h>
#include <zephyr/sys/reboot.h>
#include <hal/nrf_gpio.h>
#include <hal/nrf_power.h>
#include "sp1_board.h"
#include "controls.h"
#include "buttons.h"
#include "control_logic.h"
#include "profile.h"
#include "mapping.h"
#ifdef CONFIG_FELDD_BT_LINK
#include "bt_link.h"
#include "bt_gesture.h"
#include "bt_led.h"
#endif
#include "btn_toggle.h"
#include "cc_value.h"
#include "transport.h"
#include "midi_out.h"
#include "usbdev.h"
#include "usb_hid.h"
#include "kbd_hid.h"
#include "librarian.h"
#include "lib_bank.h"
#include "config_cdc.h"
#include "gesture.h"
#include "layer_takeover.h"
#include "dial.h"
#include "mode.h"

/* Coupling guards (spec §2.4 / §4.2): the gesture layer ceiling and the profile's
 * per-layer storage MUST agree, or a divergence silently strands the top layers.
 * Feature 4 retired the mode-flip-arm-plateau == peek-plateau coupling: the
 * dot-dot+T4 mode flip is now a genuine combo toggle (mode.c combo_dispatch), not a
 * hold-plateau gesture, so MODE_FLIP_ARM_SCANS / PEEK_HOLD_SCANS no longer exist. */
BUILD_ASSERT(GESTURE_LAYER_COUNT == NUM_LAYERS,
             "layer count must equal NUM_LAYERS");
BUILD_ASSERT(DIAL_MAX_COUNT >= GESTURE_LAYER_COUNT,
             "layer count-dial ceiling must reach the top layer");

#include "side_led.h"
#include "panic.h"
#include "wdt.h"
#include "chord_engine.h"
#include "led_override.h"
#include "led.h"
#include "clock_timer.h"
#include "clock_router.h"
#include "clockgen.h"
#include "bt_probe.h"   /* Phase-A BT bring-up probe (dev-only, CONFIG_FELDD_BT_PROBE) */
#include "bt_download.h" /* module-download state machine (dev-only, CONFIG_FELDD_BT_DOWNLOAD) */
#include "bt_provision.h" /* Q5 SS probe + DS-write provisioning (dev/canary, CONFIG_FELDD_BT_PROVISION) */

/* Feature 4: PLAY is the shift/assignable control and •• is the Fn-modifier; the
 * behavior is now a RUNTIME branch on play_mode (librarian_play_mode()), not a
 * build-time trigger. The old FELDD_SHIFT_TRIGGER_PLAY compile switch + its stock
 * (PLAY-maps-normally) arm are retired; the single arm below always treats PLAY as
 * shift-by-default (hold = momentary L2) or assignable when play_mode == 1. */

/* M5.2: the active profile that drives the mapping engine now comes from the
 * NVS-backed librarian (librarian.c), not a hardcoded g_default. The librarian
 * owns the 8 default profiles and the persisted active index; the control loop
 * reads librarian_active() (a RAM hot copy) every tick. */

/* Per-fader send-on-change state for fader_update (zero-init = un-initialized,
 * the first read seeds last_sent_raw and emits). */
static fader_t g_fader[NUM_FADERS];

/* STATEFUL button state (the on/off + run state the pure mapping engine
 * intentionally does NOT own — see profile.h / mapping.c). map_button() handles
 * the stateless BTN_NOTE / BTN_CC_MOMENTARY; the control loop owns these:
 *   - g_btn_toggle: per-LAYER, per-button latched on/off for a BTN_CC_TOGGLE
 *     (mute), flipped on the PRESS edge of the ACTIVE layer so each track-layer
 *     keeps its own bit. Reset across ALL layers on a profile change (a new
 *     profile's buttons differ, so a carried-over latch would emit a wrong CC
 *     value against the new map); NOT reset on a layer hop (independent tracks).
 *   - g_transport_playing: shared MIDI run state for BTN_TRANSPORT, threaded
 *     through transport.c's pure transport_rt() (a play/stop toggle keys off it). */
static struct btn_toggle g_btn_toggle;   /* per-LAYER, per-button latched toggles (BSS zero-init) */
static int     g_transport_playing;

/* Feature 4: dot-dot (••) Fn-modifier combo latch (per-index consume-both-edges,
 * mode.c combo_dispatch) + the per-button press-time layer latch (stuck-note-
 * across-springback fix: a NOTE pressed on shift-L2 releases its Note-Off on L2
 * even after PLAY springs back). BSS zero-init. */
static combo_latch_t g_combo;
static uint8_t       g_btn_press_layer[NUM_BUTTONS];

/* v7 chord latch state. Per-button (NUM_BUTTONS) so the two-ladder case (a chord
 * held on EACH ladder) latches independently - do NOT use a single shared latch.
 * The LATCHED set drives release Note-Offs (never the live bank), so depth/layer
 * changes mid-hold still release exactly what was pressed. */
static uint8_t g_chord_notes[NUM_BUTTONS][MAX_CHORD];
static uint8_t g_chord_count[NUM_BUTTONS];
static uint8_t g_chord_chan[NUM_BUTTONS];
/* The per-tick MIDI-out deferral ring (Section 6.4). Drained Note-Offs-first at
 * the top of each tick under CHORD_TX_BUDGET. */
static struct chord_tx_ring g_chord_tx;
/* Stage-2 per-layer last-emitted chord-depth CC (zero-init -> triad band). */
static int g_chord_depth_cc[NUM_LAYERS];
/* Tap-tempo state: the profile's assigned tap button feeds clock_tap_bpm; the gen
 * clock retempos once a 2nd tap lands (clockgen.c, host-tested). */
static struct clock_tap g_clock_tap;
/* Debounced global-BPM persistence: a fader sweep must NOT hammer NVS, so a tempo
 * change only arms a settle timer; the main loop writes the last value once it stops
 * moving. g_bpm_save_at is a k_uptime deadline (ms); 0 = nothing pending. */
static uint8_t g_bpm_pending;
static int64_t g_bpm_save_at;

/* Set the live gen tempo AND arm the ~3 s debounced global-BPM NVS save of it. The
 * assigned BPM fader + tap button both funnel through here. */
static void clock_set_live_bpm(uint16_t bpm)
{
    clock_timer_set_bpm(bpm);
    g_bpm_pending = (uint8_t)bpm;
    g_bpm_save_at = k_uptime_get() + 3000;
}

/* The active profile's GLOBAL slot (0..15, mode-aware). The clock change-detector
 * keys on THIS, not the within-bank index, so a MODE flip (MIDI<->Keyboard) - which
 * swaps to the other bank's profile but can keep the same within-index - is correctly
 * seen as a profile change and re-applies the clock config. */
static inline uint8_t clock_active_slot(void)
{
    return lib_bank_global(librarian_mode(), librarian_active_index());
}

/* The active profile's packed clock config (chord_flags[2..3]). The detector watches
 * this so a LIVE configurator edit of the active profile (enable/tap/fader/BPM synced
 * over the config protocol, which refreshes the RAM copy but does not change the slot)
 * re-applies the clock immediately - matching how a live CC edit takes effect. */
static inline uint16_t clock_active_cfg(void)
{
    const struct profile *p = librarian_active();
    return (uint16_t)(p->chord_flags[2] | ((uint16_t)p->chord_flags[3] << 8));
}

/* Re-apply the active profile's clock config (on boot or any active-profile change). */
static void clock_apply_active(bool is_boot)
{
    struct clock_cfg cc;
    profile_clock_cfg(librarian_active(), &cc);
    clock_router_apply_profile(&cc, is_boot, librarian_bpm());
}
/* Forward decl: defined just above route_midi_button, but called from
 * enter_bootloader (which appears earlier). Purges the TX ring + emits Offs. */
static void chord_flush_all(void);

/* Soft-takeover ("pickup") for LAYER switches. Per fader we remember the last CC
 * each of the NUM_LAYERS layers emitted, so on a layer step the new bank holds
 * its previous value until the physical fader CROSSES it, then resumes normal
 * snappy tracking - no value jump. The per-fader/per-layer memory lives in the
 * pure, host-tested layer_takeover.h (last_cc + valid, indexed [fader][layer]);
 * the valid flag gates the first-ever emit on a layer (seed). g_takeover_arm
 * defers the catch setup to the next fader read (which has a fresh position).
 * Profile switches DO jump (they re-arm + clear this), matching prior behavior. */
static takeover_t            g_takeover[NUM_FADERS];
static struct layer_takeover g_layer_takeover;     /* v5: per-fader, per-LAYER pickup memory */
static uint8_t               g_takeover_arm[NUM_FADERS];

/* Keyboard-mode HELD-key state (Peter auto-repeat). Tracks which buttons are
 * physically down in Keyboard mode and the (mod,key) each latched at its press
 * edge; kbd_build_report turns it into the 8-byte boot-keyboard report. A held
 * button keeps its key asserted (the host OS auto-repeats); releasing it clears
 * the slot. Cleared (+ an all-zero report sent) whenever we leave Keyboard mode
 * so no key can stick across a mode flip. */
static struct kbd_state g_kbd;

/* Rebuild the boot-keyboard report from the held set and send it (no
 * auto-release). Called after every keyboard-button edge so the host always sees
 * exactly the currently-held keys; an empty held set sends the all-zero key-up. */
static void kbd_send_held(void)
{
    uint8_t rpt[KBD_REPORT_LEN];
    kbd_build_report(&g_kbd, rpt);
    usb_hid_send_report(rpt);
#ifdef CONFIG_FELDD_BT_LINK
    bt_link_send_hid(rpt);      /* BLE-HID mirror: same report, no-op unless HID-subscribed */
#endif
}

/* Re-arm every fader's send-on-change state so the NEXT read re-emits its
 * current value even if the raw code has not moved. We call this whenever the
 * active profile changes (•• short-tap OR a host setactive/write/reset that
 * lands on a different slot): the new profile maps the same raw to a different
 * CC/range/curve, and without re-arming, fader_update's duplicate/jitter
 * suppression would keep the new mapping's values from ever reaching the
 * host/OP-XY until the user physically moved each fader. Clearing .initialized
 * forces the first post-switch read to seed + emit again. */
static void faders_rearm(void)
{
    for (int i = 0; i < NUM_FADERS; i++) {
        g_fader[i].initialized = 0;
    }
}

void k_sys_fatal_error_handler(unsigned int reason, const struct arch_esf *esf)
{ ARG_UNUSED(reason); ARG_UNUSED(esf); sys_reboot(SYS_REBOOT_COLD); CODE_UNREACHABLE; }

/* Exposed via wdt.h so long synchronous boot work (e.g. an NVS re-seed on a
 * PROFILE_VERSION bump) can keep the watchdog fed. */
void feed_wdt(void) { for (int c = 0; c < 8; c++) NRF_WDT->RR[c] = WDT_RR_RR_Reload; }

/* Make sure the hardware watchdog is actually RUNNING, not just fed. The TE
 * bootloader normally starts WDT0 (~5 s) and we only reload it - but if any boot
 * path ever hands us a STOPPED WDT, feeding RR[] is a silent no-op and we have
 * NO hang backstop at all. So start it ourselves, idempotently: if it is already
 * running (bootloader did it) leave its config untouched (a running WDT is locked
 * anyway) and just keep feeding; only if it is stopped do we configure a generous
 * ~8 s timeout (boot completes well under the bootloader's 5 s, so this can never
 * false-trip) and start it. Mirrors sp1-midi's ensure_hardware_wdt_started(). */
static void ensure_wdt_started(void)
{
    if (NRF_WDT->RUNSTATUS & WDT_RUNSTATUS_RUNSTATUS_Msk) {
        return;                                          /* already running -> just feed RR[] */
    }
    NRF_WDT->CONFIG = (WDT_CONFIG_SLEEP_Run << WDT_CONFIG_SLEEP_Pos);  /* keep counting in CPU sleep */
    NRF_WDT->CRV    = 8u * 32768u - 1u;                  /* ~8 s @ 32.768 kHz LFCLK */
    NRF_WDT->RREN   = WDT_RREN_RR0_Msk;                  /* reload channel 0 (feed_wdt writes RR[0..7]) */
    NRF_WDT->TASKS_START = 1u;
}

/* Enable the SP-1's BQ24232 battery charger by driving /CE (P0.21) low, and set
 * the two open-drain status lines as pulled-up inputs. WITHOUT this the battery
 * never charges - even while USB is plugged in - so the cell drains flat and the
 * device browns out at random (at boot, mid-flash, mid-session). Mirrors the
 * chattock sp1-tape-looper's charger_init(); pins are TimK's verified pinout. */
static void charger_init(void)
{
    nrf_gpio_pin_clear(SP1_CHG_NCE);                   /* drive low BEFORE switching to output */
    nrf_gpio_cfg_output(SP1_CHG_NCE);
    nrf_gpio_pin_clear(SP1_CHG_NCE);                   /* /CE low = charging enabled */
    nrf_gpio_cfg_input(SP1_CHG_NCHG,   NRF_GPIO_PIN_PULLUP);
    nrf_gpio_cfg_input(SP1_CHG_NPGOOD, NRF_GPIO_PIN_PULLUP);
}

/* M3.2 TRS ring = PNP current-source base on P0.23, active-LOW: driving the
 * pin electrically HIGH turns the current source OFF (parks the opto). */
#define SP1_TRS_RING  NRF_GPIO_PIN_MAP(0, 23)

static void enter_bootloader(void)
{
    /* v7: release any held chord notes BEFORE the power-off teardown so a
     * TRS-attached synth doesn't ring through SYSTEM_OFF. Idempotent + purges the
     * TX ring first (Fix 6). */
    chord_flush_all();

#ifdef CONFIG_FELDD_BT_LINK
    bt_link_suspend();  /* hold the module in reset before SYSTEM_OFF */
#endif

    /* F6: SYSTEM_OFF on nRF52840 only wakes on a configured DETECT/sense, a
     * full reset, or USB. To return from power-off on a •• press we MUST arm a
     * sense-low wake source on the •• pin BEFORE entering off. We also park the
     * output pins (LEDs, BTN_COM rail, TRS ring) to cut leakage in the off
     * state. Order matters: teardown outputs, ARM the wake-sense pin, then clear
     * RESETREAS, then SYSTEMOFF. We never touch P0.27's config after arming it. */

    /* Park output pins low / disable the TRS ring. LEDs off (clear = off). */
    led_pin(SP1_LED1, false);
    led_pin(SP1_TRACK_LED1, false);
    led_pin(SP1_TRACK_LED2, false);
    led_pin(SP1_TRACK_LED3, false);
    led_pin(SP1_TRACK_LED4, false);
    /* Park the other 3 side (play) LEDs too (P0.01/P1.12/P0.00). Without this the
     * lit LAYER / mode-flash pattern (0.9.1) stays driven and is RETAINED through
     * SYSTEM_OFF, draining the cell (the 0.7.0-beta "LEDs never turn off" report).
     * SP1_PLAY_LED4 == SP1_LED1 is already cleared above. Skipped under
     * -DFELDD_MODE_LED_SINGLE where these 3 pins are never driven. */
#ifndef FELDD_MODE_LED_SINGLE
    led_pin(SP1_PLAY_LED1, false);
    led_pin(SP1_PLAY_LED2, false);
    led_pin(SP1_PLAY_LED3, false);
#endif
    /* BTN_COM ladder/fader supply: drive low to stop powering the resistor
     * ladders in the off state. */
    nrf_gpio_cfg_output(SP1_BTN_COM);
    nrf_gpio_pin_clear(SP1_BTN_COM);
    /* TRS ring is DT-active-low: drive the pin HIGH to switch the PNP current
     * source OFF (logical-disabled) before sleep. */
    nrf_gpio_cfg_output(SP1_TRS_RING);
    nrf_gpio_pin_set(SP1_TRS_RING);

    /* The •• button is still physically held LOW here (we only reach this after
     * a ~5 s hold). If we armed the level-sense now, the already-low pin would
     * re-assert DETECT the instant SYSTEM_OFF latches and wake/reset us straight
     * back into the app -- the power-off would silently fail. Wait for release
     * first, feeding the bootloader WDT so it doesn't reset us mid-wait, then
     * debounce. Mirrors the looper's power_off(). */
    while (nrf_gpio_pin_read(SP1_FUNC_BTN) == 0) { feed_wdt(); k_msleep(20); }
    k_msleep(60);   /* debounce the release */

    /* Arm the •• function button (P0.27) as a sense-low wake source: input,
     * pull-up, SENSE=Low. A press pulls it low -> DETECT wakes from SYSTEM_OFF.
     * nrf_gpio_cfg_sense_input writes PIN_CNF in one shot (dir/pull/sense). */
    nrf_gpio_cfg_sense_input(SP1_FUNC_BTN, NRF_GPIO_PIN_PULLUP,
                             NRF_GPIO_PIN_SENSE_LOW);

    NRF_POWER->RESETREAS = 0xFFFFFFFFu;
    __DSB();
    NRF_POWER->SYSTEMOFF = 1u;
    __DSB();
    for (;;) { }
}

/* FAILSAFE recovery (Track1+4 held ~1.2 s): reset into the bootloader so the
 * device can ALWAYS be reflashed. Light all 4 track LEDs as the "loading
 * firmware" cue, write the UF2 magic (harmless if the bootloader ignores it)
 * and reset; the bootloader's own button scan enters DFU. Mirrors the looper's
 * enter_dfu(). */
static void enter_dfu(void)
{
    led_pin(SP1_TRACK_LED1, true);
    led_pin(SP1_TRACK_LED2, true);
    led_pin(SP1_TRACK_LED3, true);
    led_pin(SP1_TRACK_LED4, true);
    NRF_POWER->GPREGRET = 0x57u;
    __DSB();
    NVIC_SystemReset();
    for (;;) { }
}


/* (0.9.1 LED redesign) The old BLOCKING mode_confirm_chase() directional LED
 * chase is DELETED. A mode flip no longer runs a ~480 ms k_msleep chase on the
 * play row (which dropped MIDI/HID/fader edges for the whole window). The SIDE
 * row now permanently shows the LAYER (one LED) and a mode switch arms a brief,
 * NON-BLOCKING per-tick MODE-FLASH of the mode pattern (side_led_pattern, armed
 * via mode_flash_ticks = MODE_FLASH_TICKS at the two switch sites) that yields
 * back to the layer LED on its own. See the side-row render in the control loop. */

/* BQ24232 status reads (open-drain, low = active), per TimK's wiki. */
static int usb_present(void) { return nrf_gpio_pin_read(SP1_CHG_NPGOOD) == 0; }
static int charging(void)    { return nrf_gpio_pin_read(SP1_CHG_NCHG)   == 0; }

/* Battery gauge (charge-standby "filling bar", mirrors stock). controls_read_raw(6)
 * = battery on AIN4/P0.28, gain 1/6, ref 0.6 V internal, behind the on-board divider.
 * The divider ratio is undocumented, so BATT_RAW_EMPTY/FULL are CALIBRATED from the
 * "BATT raw=" RTT print against the stock curve (empty ~3.45 V / full ~4.18 V).
 * PROVISIONAL (divider assumed ~1/2) until the on-rig RTT read confirms. */
#define BATT_RAW_EMPTY 1962
#define BATT_RAW_FULL  2378
static int battery_pct(int raw)
{
    if (raw < 0) {
        return -1;
    }
    int pct = (raw - BATT_RAW_EMPTY) * 100 / (BATT_RAW_FULL - BATT_RAW_EMPTY);
    if (pct < 0)   pct = 0;
    if (pct > 100) pct = 100;
    return pct;
}

/* Render the 4-bar charge gauge on the SIDE/play row. Bar 1 = idx 4 (SP1_PLAY_LED1 =
 * the layer-0/default-profile LED = the physical "first" LED, USER-confirmed), filling
 * up to idx 7. While CHARGING: N quarters solid + the next one blinks ("filling"), rest
 * off. Charge COMPLETE (nCHG released): all 4 solid. */
static void charge_gauge(int pct, int chg, uint32_t blink)
{
    if (!chg) {                                  /* full / charge complete -> all solid */
        for (int i = 0; i < 4; i++) {
            led_idx(4 + i, true);
        }
        return;
    }
    int n_full = (pct < 0) ? 0 : pct / 25;       /* 0..4 solid quarters */
    if (n_full > 4) n_full = 4;
    int blink_on = ((blink / 12u) & 1u);         /* ~0.5 s period at 40 ms/tick */
    for (int i = 0; i < 4; i++) {
        int on;
        if (i < n_full)                     on = 1;         /* filled  */
        else if (i == n_full && n_full < 4) on = blink_on;  /* filling (blinks) */
        else                                on = 0;         /* empty   */
        led_idx(4 + i, on);
    }
}

/* CHARGE-STANDBY GATE (mirrors the chattock sp1-tape-looper). The SP-1
 * bootloader hands control to the app on ANY power event: a deliberate ••
 * power-on, but ALSO a bare USB-charge plug-in, a battery insert, OR the
 * soft-reset right after a flash. Only a •• wake (RESETREAS.OFF) or a watchdog
 * recovery (RESETREAS.DOG) is a "real" turn-on. For anything else we must NOT
 * spin up the full app (USB, SAADC, TRS) on what may be a nearly-flat cell -
 * that is exactly what brown-out-thrashes a low battery and can wedge the
 * device out of reach of the bootloader (the bug that bricked Unit A on the
 * bench). So park here: show the battery gauge (a stock-style filling bar on the
 * side row) while charging, wait for a ~0.6 s •• hold to actually switch on, and on
 * battery with nothing held drop to a clean SYSTEM_OFF (a •• press wakes it). */
static void charge_standby_gate(uint32_t wake_reas)
{
    /* Real power-on (•• press) or watchdog recovery -> straight to full boot. */
    if (wake_reas & (POWER_RESETREAS_OFF_Msk | POWER_RESETREAS_DOG_Msk)) {
        return;
    }

    int64_t hold_t = -1;
    uint32_t tick = 0;
    int adc_up = 0;
    int last_raw = -1;
    for (;;) {
        feed_wdt();
        if (nrf_gpio_pin_read(SP1_FUNC_BTN) == 0) {   /* •• pressed */
            if (hold_t < 0) {
                hold_t = k_uptime_get();
            } else if (k_uptime_get() - hold_t >= 600) {
                break;                                /* held ~0.6 s -> power on */
            }
            led_idx(0, true);                         /* press feedback on track LED1 (clear of the side gauge) */
        } else {                                      /* •• released */
            hold_t = -1;
            if (!usb_present()) {
                enter_bootloader();                   /* on battery + idle -> SYSTEM_OFF */
            }
            led_idx(0, false);                        /* clear press feedback */
            /* Charging: full-brightness battery gauge on the side row (a charge
             * indicator must be glanceable; 50% is too dim on these small LEDs). The
             * SAADC + BTN_COM rail only spin up on USB (charging) — a battery-idle
             * standby drops to SYSTEM_OFF above, so this never touches them on a flat
             * cell. The cell charges slowly, so re-sample only ~every 2 s; the blink
             * is LED-only. NOTE: the charge-standby park is reset to full boot by the
             * TE bootloader after ~60-120 s (a bootloader-level app-confirm timeout,
             * NOT this loop — a bare blink-only gate does the same), so the gauge shows
             * for that window per plug-in until that is addressed at the bootloader level. */
            if (!adc_up) {
                controls_init();
                led_set_brightness(LED_BRIGHTNESS_FULL);
                last_raw = controls_read_raw(6);
                adc_up = 1;
            } else if ((tick % 50u) == 0u) {
                last_raw = controls_read_raw(6);
            }
            charge_gauge(battery_pct(last_raw), charging(), tick);
        }
        k_msleep(40);
        tick++;
    }

    led_set_brightness(LED_BRIGHTNESS_DEFAULT);       /* restore ambient dim for normal operation */
    for (int i = 0; i < 4; i++) {                     /* clear the side gauge on exit */
        led_idx(4 + i, false);
    }
    led_idx(0, false);
    /* Wait for the •• release so the power-on hold doesn't bleed into the run
     * loop's •• tap/hold gesture handling. */
    while (nrf_gpio_pin_read(SP1_FUNC_BTN) == 0) { feed_wdt(); k_msleep(20); }
}

/* M6.2: the USB CDC ACM console is now the structured CONFIG PROTOCOL channel,
 * owned by config_cdc.c. The per-loop raw-ADC debug stream (controls_diag) and
 * the per-event MIDI/BTN printk chatter have been removed so this port is a
 * clean newline-delimited JSON stream (protocol responses + monitor frames)
 * that the web config tool can parse. printk remains available for fatal/boot
 * diagnostics only (it shares this UART, so we keep it off the fast path). */

/* Boot animation: a quick track-LED 1->2->3->4 sweep, run once right after the
 * charge-standby gate, so feldd visibly announces itself at power-on. (It began
 * life as a CDC-bring-up version marker; kept as a deliberate flourish.) */
#define FELDD_BOOT_SIG_SWEEPS 2   /* feldd v0.6.2-beta: 2 sweeps */
static void boot_signature(void)
{
    led_set_brightness(LED_BRIGHTNESS_FULL);
    const uint32_t leds[4] = {
        SP1_TRACK_LED1, SP1_TRACK_LED2, SP1_TRACK_LED3, SP1_TRACK_LED4
    };
    for (int i = 0; i < 4; i++) {
        led_pin(leds[i], false);
    }
    for (int pass = 0; pass < FELDD_BOOT_SIG_SWEEPS; pass++) {
        for (int i = 0; i < 4; i++) {
            feed_wdt();
            led_pin(leds[i], true);
            k_msleep(80);
            led_pin(leds[i], false);
        }
    }
    led_set_brightness(LED_BRIGHTNESS_DEFAULT);
}

/* Emit a Note-Off for every latched chord note on every button, then zero the
 * latches. Idempotent (zero-count latches emit nothing), so it is safe on any
 * reset/interruption path. Goes straight to midi_out_send (NOT the ring): a
 * flush is a stop-everything event, so we want the offs out immediately, and a
 * flush emits at most 9*MAX_CHORD offs only in a pathological all-held case.
 *
 * Fix 6 - ORDERING RULE: flush PURGES the ring. The press/release path enqueues
 * On/Off onto g_chord_tx (drained next tick), so on a map change the ring may
 * still hold On/Off messages for the now-stale map. If we emitted the direct
 * Offs but left the ring intact, a queued Note-On could drain AFTER this flush's
 * Offs (the ring drains at the TOP of the next tick) and re-strand a note. So we
 * reset the ring HERE, discarding any queued On/Off for the stale map, BEFORE
 * the direct Offs go out. (Discarded queued Offs are harmless - the direct Offs
 * below already release everything that was latched.) */
static void chord_flush_all(void)
{
    chord_tx_init(&g_chord_tx);   /* Fix 6: purge any queued On/Off for the stale map FIRST */
    for (int idx = 0; idx < NUM_BUTTONS; idx++) {
        for (int n = 0; n < g_chord_count[idx]; n++) {
            struct midi_msg m = {
                .status = (uint8_t)(0x80 | (g_chord_chan[idx] & 0x0F)),
                .d1 = g_chord_notes[idx][n], .d2 = 0, .len = 3,
            };
            midi_out_send(&m, NULL);
        }
        g_chord_count[idx] = 0;
    }
}

/* Queue one chord message onto the per-tick ring (drained under the cap). */
static void chord_tx_enqueue(uint8_t status, uint8_t d1, uint8_t d2)
{
    (void)chord_tx_push(&g_chord_tx, status, d1, d2);
}

/* Route one button edge to MIDI, owning the STATEFUL button types the pure
 * mapping engine deliberately leaves to the control loop (mapping.c: BTN_CC_TOGGLE
 * / BTN_TRANSPORT / BTN_PROFILE_SWITCH emit nothing). The stateless types
 * (BTN_NOTE, BTN_CC_MOMENTARY, BTN_NONE) are delegated straight to map_button(),
 * unchanged. The three stateful types act on the PRESS edge ONLY (pressed != 0)
 * so one physical press = one action (release does nothing) — matching how the
 * old map_button no-op behaved (release of a momentary still flows through
 * map_button below). Returns 1 if it switched the active profile (so the caller
 * can refresh its active tracking + reset the toggle latches), else 0.
 *
 * The CC NUMBER for a toggle is the button's `value` field (same convention as
 * map_button's BTN_CC_MOMENTARY) and we honor the per-button channel + the active
 * LAYER (0..NUM_LAYERS-1) bank exactly like map_button. */
static int route_midi_button(int idx, int pressed, int layer_now)
{
    const struct profile *p = librarian_active();
    /* v6: the button TYPE + channel are read from the ACTIVE layer's bank (L1
     * inline, L2/L3/L4 from ext[]), so a stateful button on a cycled layer honors
     * THAT layer's type/channel — matching map_button's per-layer reads. */
    switch (profile_layer_button_type(p, idx, layer_now)) {
    case BTN_CC_TOGGLE:
        if (pressed) {
            uint8_t cc = profile_layer_button_value(p, idx, layer_now);
            /* CC#0 = unbound sentinel (mirrors map_fader's cc==0 + map_button's CC_MOMENTARY
             * guard): an unmapped toggle left on CC0 must NOT fire CC0 or flip its latch. */
            if (cc == 0) return 0;
            uint8_t on = btn_toggle_flip(&g_btn_toggle, layer_now, idx);
            struct midi_msg m = {
                .status = (uint8_t)(0xB0 | (profile_layer_button_channel(p, idx, layer_now) & 0x0F)),
                .d1     = cc,
                .d2     = on ? 127 : 0,
                .len    = 3,
            };
            midi_out_send(&m, NULL);
        }
        return 0;
    case BTN_TRANSPORT:
        if (pressed) {
            /* transport.c (pure): value selects Start/Stop/Continue/toggle and
             * threads g_transport_playing; emit the single-byte real-time status
             * (len 1 -> trs_send writes ONLY the status; midi1_event encodes the
             * CIN-0xF single-byte USB event). */
            uint8_t rt = transport_rt(profile_layer_button_value(p, idx, layer_now), &g_transport_playing);
            struct midi_msg m = { .status = rt, .d1 = 0, .d2 = 0, .len = 1 };
            midi_out_send(&m, NULL);
        }
        return 0;
    case BTN_PROFILE_SWITCH:
        if (pressed) {
            /* value 0 = NEXT within bank, value 1 = PREV. Both stay inside the
             * current mode's bank of 8 (never cross banks — that is a mode flip):
             * NEXT via lib_bank_cycle (wraps 7->0); PREV wraps 0->NUM_BANK_PROFILES-1.
             * The caller's now_active!=last_active block re-arms the faders for the
             * new profile's CCs; we reset the toggle latches here because a new
             * profile has different buttons. */
            uint8_t cur  = librarian_active_index();
            uint8_t next = (profile_layer_button_value(p, idx, layer_now) == 1)
                ? (uint8_t)(cur == 0 ? (NUM_BANK_PROFILES - 1) : (cur - 1))
                : lib_bank_cycle(cur);
            if (librarian_set_active(next) == 0) {
                chord_flush_all();   /* v7: release held chords before the new map */
                btn_toggle_reset_all(&g_btn_toggle);
                return 1;
            }
        }
        return 0;
    case BTN_CHORD: {
        struct chord_def cdef;
        const struct chord_def *c = profile_layer_chord(p, idx, layer_now, &cdef);
        uint8_t ch = profile_layer_button_channel(p, idx, layer_now) & 0x0F;
        if (pressed) {
            if (!c) return 0;                 /* index 0 -> plays nothing */
            /* Stage 2: read this layer's cached depth CC; chord_resolve gates it
             * off for non-eligible qualities / explicit / range. */
            int depth = chord_depth_from_cc(g_chord_depth_cc[layer_now]);
            uint8_t notes[MAX_CHORD];
            int n = chord_resolve(c, depth, notes);
            /* Latch the EXACT emitted set + channel for a correct release. */
            for (int k = 0; k < n; k++) g_chord_notes[idx][k] = notes[k];
            g_chord_count[idx] = (uint8_t)n;
            g_chord_chan[idx]  = ch;
            uint8_t vel = p->chord_flags[0];  /* per-profile chord velocity */
            for (int k = 0; k < n; k++)
                chord_tx_enqueue((uint8_t)(0x90 | ch), notes[k], vel);   /* low to high */
        } else {
            for (int k = 0; k < g_chord_count[idx]; k++)
                chord_tx_enqueue((uint8_t)(0x80 | g_chord_chan[idx]),
                                 g_chord_notes[idx][k], 0);
            g_chord_count[idx] = 0;
        }
        return 0;
    }
    case BTN_CC_VALUE: {
        /* Feature 1: this button REUSES its chord6 slot for {sub,on,off}; branching
         * here (before BTN_CHORD) guarantees the CC-value bytes are NEVER fed to the
         * chord engine. cc_value_decide owns the 3 sub-modes and threads the shared
         * g_btn_toggle latch for the toggle sub-mode. The CC NUMBER rides `value`. */
        uint8_t cc = profile_layer_button_value(p, idx, layer_now);
        /* CC#0 = unbound sentinel (like the fader + CC_MOMENTARY/CC_TOGGLE): skip an unmapped
         * cc-value button entirely - do not decide/latch or fire CC0. */
        if (cc == 0) return 0;
        uint8_t sub, on, off, d2;
        if (profile_layer_ccval(p, idx, layer_now, &sub, &on, &off) != 0) return 0;
        if (!cc_value_decide(sub, pressed, on, off, &g_btn_toggle, layer_now, idx, &d2)) return 0;
        struct midi_msg m = {
            .status = (uint8_t)(0xB0 | (profile_layer_button_channel(p, idx, layer_now) & 0x0F)),
            .d1     = cc,   /* CC number stays in `value` */
            .d2     = d2,
            .len    = 3,
        };
        midi_out_send(&m, NULL);
        return 0;
    }
    /* Stateless types (and NONE): the pure engine still owns these. map_button
     * emits NOTE on press/release and CC_MOMENTARY 127/0 on press/release. */
    case BTN_NOTE: case BTN_CC_MOMENTARY: case BTN_NONE: default:
        map_button(p, idx, pressed, layer_now, midi_out_send, NULL);
        return 0;
    }
}

int main(void)
{
    uint32_t wake_reas = NRF_POWER->RESETREAS;   /* WHY did we boot? read BEFORE clearing */
    NRF_POWER->RESETREAS = 0xFFFFFFFFu;   /* clear on boot */
    NRF_POWER->GPREGRET = 0;              /* TKT rule: clear GPREGRET too. A stale DFU flag
                                          * (our enter_dfu writes 0x57) left set could make
                                          * the bootloader misfire after a watchdog reset; a
                                          * clean GPREGRET lets the WDT be the real safety net. */

    ensure_wdt_started();   /* guarantee a hang backstop instead of trusting the bootloader's WDT */
    /* Bring up the unified PWM LED layer (all 8 channels) ONCE, before any led_*
     * drive and before the charge-standby gate below. PWM (via pinctrl) now owns
     * the LED pins, so we no longer configure SP1_LED1 / the 3 side play LEDs as
     * raw GPIO outputs here — led_init() replaces the whole LED cfg_output block. */
    led_init();
    nrf_gpio_cfg_input(SP1_FUNC_BTN, NRF_GPIO_PIN_PULLUP);

#if defined(CONFIG_FELDD_BT_LINK)
    /* Hold the CYW20706 in reset (BT DEFAULT OFF) from the very first boot instant, BEFORE the
     * charge-standby gate below can SYSTEM_OFF. The nRF52840 RETAINS driven GPIO state through
     * SYSTEM_OFF, so this clamp survives a bare-charge session — the module never advertises while
     * the device is "off and charging". Gated on CONFIG_FELDD_BT_LINK; the probe/download builds
     * own P0.10 themselves and run before the gate. bt_link_init re-affirms this on the normal path. */
    nrf_gpio_cfg_output(NRF_GPIO_PIN_MAP(0, 10));   /* module RST_N */
    nrf_gpio_pin_clear(NRF_GPIO_PIN_MAP(0, 10));    /* HELD IN RESET = BT OFF */
#endif

    charger_init();         /* enable battery charging ASAP so a low cell can't brown us out */

#ifdef CONFIG_FELDD_BT_PROBE
    /* Phase-A Bluetooth bring-up probe (dev-only). Runs HERE — before the
     * charge-standby gate — so a plain SWD --reset (SREQ, which the gate would
     * park) still runs it with NO •• wake needed on the USB-powered burner. The WDT
     * is already started (above) and the probe feeds it; LEDs + charger are up. It
     * never returns, so the gate + normal control loop below are skipped. The probe
     * itself is a light load (one UART + a few GPIOs, no USB/SAADC/audio), so it
     * does not reintroduce the flat-cell brown-out the gate guards against. */
    bt_probe_run();
#endif

#ifdef CONFIG_FELDD_BT_DOWNLOAD
    /* Module-download state machine (dev-only). Like the probe, runs HERE — before
     * the charge-standby gate — so a plain SWD --reset drives it with NO •• press on
     * the USB-powered burner. Same light load as the probe (one UART + a few GPIOs);
     * it never returns. It is DS-only + SS-preserving: even a brown-out mid-write can
     * only leave a re-flashable DS, and never touches the SS (which is never written).
     * Mutually exclusive with the probe above. */
    bt_download_run();
#endif

    /* Park in low-power charge-standby unless this was a deliberate •• power-on
     * or a watchdog recovery, so a full boot can never brown-out-thrash a low
     * cell (the failure that wedged Unit A). Returns only on a real turn-on. */
#ifndef CONFIG_FELDD_REMOTE_HID_TEST
    charge_standby_gate(wake_reas);
#else
    (void)wake_reas;        /* remote HID bench test: skip the park, always full-boot on an SWD reset */
#endif

    boot_signature();       /* feldd boot animation: track-LED 1->2->3->4 sweep */

    controls_init();        /* power the ladder/fader rail + set up SAADC channels */
    buttons_init();         /* reset the ladder decode/debounce state */
    chord_tx_init(&g_chord_tx);  /* v7: empty the chord MIDI-out deferral ring at boot */
    midi_out_init();        /* bring up uart1 TRS MIDI out + enable the ring PNP */
    clock_timer_init();     /* MIDI clock generator (GEN mode) */
    clock_router_init();    /* GEN/THRU selector: USB-in clock -> THRU, else GEN */
    usbdev_start();         /* enumerate the USB composite: CDC console + USB-MIDI */

#ifdef CONFIG_FELDD_BT_LINK
    bt_link_init();     /* PREPARE the runtime link only; module HELD IN RESET (BT default OFF) until •• + play */
#ifdef CONFIG_FELDD_REMOTE_HID_TEST
    bt_link_bt_on();    /* remote HID bench test: cold-boot the module + advertise NOW, no •• + play gesture */
#endif
#endif

    /* M5.2: mount NVS, lay down 8 default profiles on first boot, and load the
     * persisted active profile into the librarian's RAM hot copy. If NVS fails
     * to come up (e.g. an unexpected flash fault), the control loop below still
     * runs against whatever librarian_active() returns — librarian_init zeroes
     * its state, so map_* sees a benign empty profile rather than crashing. */
    int lib_rc = librarian_init();
    printk("LIB init rc=%d active=%d\n", lib_rc, librarian_active_index());

    /* Feature B (0.23): apply the persisted LED brightness. The charge-standby gate
     * left us at the ambient default, so honor a user "full" preference here. */
    led_set_brightness(librarian_brightness() ? LED_BRIGHTNESS_FULL : LED_BRIGHTNESS_DEFAULT);

    /* Clock: apply the active profile's clock config now that the librarian has
     * loaded. is_boot=true keeps the restored global tempo (a power cycle resumes the
     * last-used tempo, not a snap to the profile default). A clock-less/legacy profile
     * decodes to disabled, so the clock stays silent unless the profile opted in. */
    clock_apply_active(true);

    /* M6.2: bind the CDC console as the JSON-lines config protocol channel and
     * start its line reader. From here on the control loop calls
     * config_cdc_poll() each tick to service host requests and emits monitor
     * frames for fader CCs / button edges when monitoring is enabled. */
    int cdc_rc = config_cdc_init();
    printk("CDC init rc=%d\n", cdc_rc);

    /* The loop runs at ~8 ms so the button ladders scan + debounce at the
     * cadence buttons.c expects (3-read debounce ~24 ms, DFU hold ~1.2 s). The
     * slower behaviours below are re-throttled to that tick:
     *   •• hold ~5 s   -> 625 ticks    LED heartbeat ~250 ms -> every 31 ticks
     *   •• short tap   -> held 1..40 ticks (<= ~320 ms)
     *
     * •• FUNCTION BUTTON GESTURE (spec §8 count-dial — replaces the old single-tap
     * round-robin):
     *   - HOLD ~5 s (>= 625 ticks)  -> SYSTEM_OFF back to bootloader (power off).
     *     RAISED from ~3 s / 375 ticks by the #61 gesture pass (spec
     *     notes/2026-06-20-feldd-gesture-pass-spec.md §4) so a shorter •• hold is
     *     provably free for a future gesture with zero accidental-shutdown risk:
     *     the device cannot power off until •• is held a full ~5 continuous
     *     seconds. The freed 75..624-tick (~0.6..~5 s) band is RESERVED (no action
     *     this pass; see §3).
     *   - SHORT TAP (held 1..40 ticks, i.e. <= ~320 ms, then released) is fed to
     *     the dial FSM (dial.c) as tap_edge=1. The dial then decides:
     *        1 tap        -> RELATIVE +1 (lib_bank_cycle, the old round-robin)
     *        2..8 taps    -> ABSOLUTE jump to within-bank profile N (count-1).
     *   This re-uses the single •• button: a quick dot-dot dial cycles/jumps
     *   profiles, a long press powers off. The tap upper bound is TIGHTENED from
     *   <62 to <=40 ticks (spec §8) to widen the margin from rapid dialing to the
     *   power-off hold; the dead band is now 41..624 ticks (does nothing on
     *   release), so a "medium" press the user aborts before the ~5 s power
     *   threshold neither dials a profile nor powers off — avoids accidental
     *   switches while the user is on their way to a power-off hold. */
    uint32_t held = 0;
    uint32_t tick = 0;
    /* Honor the •• button only AFTER it has read released (high) at least once
     * since boot. On a low/marginal battery the pin can read low at power-on
     * (stuck, noisy, or brownout-glitched); without this guard that would count
     * straight toward the ~5 s power-off and kill the boot. */
    int func_armed = 0;
#ifdef CONFIG_FELDD_BT_LINK
    bt_gesture_t bt_gest; bt_gesture_init(&bt_gest);
    int  bt_anim_pat       = BT_LED_NONE;  /* active momentary BT LED pattern */
    int  bt_anim_tick      = 0;
    int  bt_midi_ready_was = 0;            /* rising-edge latch for the CONNECTED double-flash */
    /* NOTE: no func_combo / play_combo_consumed locals — the hand-rolled •• + play
     * suppression that guarded the retired PLAY peek/dial FSM is gone. On 0.24 the
     * •• + play co-hold reuses the combo engine's consume-both-edges + reset_hold
     * (via g_combo's idx-0 arm in the evt loop below). No saved-brightness local
     * either — the BT LED player is FRONT-ROW ON/OFF ONLY and never touches the GLOBAL
     * led_set_brightness() (see (f)), so there is nothing to save/restore. */
#endif
    /* Track the active profile index across ticks so we can re-arm the faders
     * uniformly on ANY active-slot change — a •• short-tap, or a host
     * setactive/write/reset that lands on a different slot — without each call
     * site having to remember to do it. Seed from the boot-loaded active index
     * so the first loop iteration doesn't spuriously re-arm. */
    uint8_t last_active = clock_active_slot();   /* mode-aware active-profile key (0..15) */
    uint16_t last_clk = clock_active_cfg();      /* active profile's clock bytes (live-edit watch) */
    /* PLAY engaged-layer holder (Feature 4). gesture_t now carries ONLY the engaged
     * layer (the peek FSM is retired): the layer is SET by the ••+FWD/RWD combo
     * (gesture_set_layer(gesture_layer_step(...))) and READ by the side-row LED +
     * effective_layer(). PLAY itself is the shift/assignable control, dispatched
     * per-edge in the evt loop below. */
    gesture_t shift_gesture;
    gesture_init(&shift_gesture);
    /* dot-dot (••) Fn-modifier combo latch (per-index consume-both-edges). */
    combo_latch_init(&g_combo);
    /* v5: per-fader, per-LAYER soft-takeover pickup memory. Statics already zero
     * (valid=0), but init explicitly so the intent is local to the soft-takeover
     * state and survives any future move off file scope. */
    layer_takeover_init(&g_layer_takeover);
    /* (0.9.1 LED redesign) SIDE-row MODE-FLASH state. The side row permanently
     * shows the LAYER (one LED at gesture_layer()); a successful mode switch arms
     * a brief, NON-BLOCKING flash of the NEW mode's pattern (captured in
     * mode_flash_mode), counted down by mode_flash_ticks exactly like the proven
     * front-row dial_confirm_ticks deadline. When it hits 0 the side row
     * re-asserts the layer LED on the next tick automatically. The replacement for
     * the deleted blocking mode_confirm_chase(). */
    int           mode_flash_ticks  = 0;
    unsigned char mode_flash_mode   = 0;
    /* Feature B (0.23): ••+T3 MIDI-panic confirm flash. >0 lights ALL track + side
     * LEDs for a brief window (highest LED priority), counted down per tick like the
     * mode flash. */
    int           g_panic_flash_ticks = 0;
    const int     PANIC_FLASH_TICKS   = 4;   /* brief all-LED panic confirm flash */
    /* "Calm middle" tunables (A/B on hardware without code surgery, per the
     * approved decision). MODE_FLASH_TICKS = flash window length (~150 ticks ~=
     * 1.2 s @ ~8 ms/tick). MODE_FLASH_STYLE = 0 PULSE (solid pattern the whole
     * window, the calm default) / 1 BLINK (toggle on (tick/12)&1). */
    const int     MODE_FLASH_TICKS  = 150;   /* ~1.2 s non-blocking mode-flash */
    const int     MODE_FLASH_STYLE  = 0;     /* 0 = PULSE (solid), 1 = BLINK */
    /* v7: last CDC DTR (host-open) state, for the edge-triggered chord flush on a
     * host disconnect. Starts 0 so a never-connected host never spuriously flushes. */
    static int    g_last_dtr = 0;
    for (;;) {
        feed_wdt();
#ifdef CONFIG_FELDD_BT_LINK
        bt_link_poll();
#ifdef CONFIG_FELDD_REMOTE_HID_TEST
        /* Remote HID bench test: once a host has subscribed (bit2 in g_status), emit a periodic
         * 'x' keystroke over BLE-HID so the HOGP delivery path can be verified with no buttons.
         * bt_link_send_hid() no-ops until HID-subscribed, so this stays silent until a host attaches. */
        {
            static uint32_t rht = 0;
            if      ((rht % 250) == 0)  { uint8_t r[8] = {0,0,0x1B,0,0,0,0,0}; bt_link_send_hid(r); }  /* press 'x' (HID 0x1B) */
            else if ((rht % 250) == 12) { uint8_t r[8] = {0,0,0,0,0,0,0,0};    bt_link_send_hid(r); }  /* key-up ~100 ms later */
            rht++;
        }
#endif
#endif

        /* Drain the chord TX ring FIRST (Note-Offs prioritized) under the per-tick
         * cap so a multi-chord burst clears deterministically across ticks without
         * blowing the 8 ms tick / WDT. */
        {
            struct chord_tx_msg txo[CHORD_TX_BUDGET];
            int nd = chord_tx_drain(&g_chord_tx, txo);
            for (int i = 0; i < nd; i++) {
                struct midi_msg m = { .status = txo[i].status, .d1 = txo[i].d1,
                                      .d2 = txo[i].d2, .len = 3 };
                midi_out_send(&m, NULL);
            }
        }

        /* Service the CDC config protocol: drain any host request lines and
         * write their responses. Non-blocking (returns at once if no RX). */
        config_cdc_poll();

        /* On a host disconnect (DTR drop) flush held chords so a TRS-attached
         * synth doesn't ring forever (USB offs go nowhere once the host is gone,
         * but TRS keeps working). Edge-triggered: flush only on the 1->0 transition. */
        {
            int dtr = config_cdc_dtr();
            if (g_last_dtr && !dtr) chord_flush_all();
            g_last_dtr = dtr;
        }

        /* •• function button (Feature 4): it is now the Fn-MODIFIER level, not a
         * profile count-dial. A BARE hold past 625 ticks (~5 s) powers off; a hold
         * with ANY consumed combo press does NOT — combo_dispatch resets `held` to
         * 0 on each consumed press (the clean-hold power-off gate, spec (b)4).
         * func_down exposes the modifier level to the evt loop + LED peek below. */
        int func_low = (nrf_gpio_pin_read(SP1_FUNC_BTN) == 0);
        if (!func_armed) {
            /* Not armed yet: ignore •• entirely until it reads released once. */
            if (!func_low) {
                func_armed = 1;
            }
            held = 0;
        } else if (func_low) {
            /* Accumulate the hold. Only a BARE hold reaches 625: a consumed combo
             * press resets `held` to 0 in the evt loop below, so ••+combo never
             * crosses the power-off threshold. On the BT build the •• + play co-hold
             * is one such consumed combo (idx-0 arm in the evt loop), so it likewise
             * voids the power-off — replacing the retired hand-rolled func_combo freeze. */
#ifdef CONFIG_FELDD_BT_LINK
            /* •• + play co-hold: freeze the power-off counter EVERY tick it is active.
             * The evt-loop idx-0 arm resets `held` only at the PLAY edge, so a sustained
             * hold (e.g. past the 2.4 s fill-to-confirm FORGET) would otherwise re-accumulate
             * to 625 and power off. bt_gesture_active() (co_ticks>0) guarantees •• + play never
             * powers off regardless of hold length. */
            if (bt_gesture_active(&bt_gest)) {
                held = 0;
            } else
#endif
#ifdef CONFIG_FELDD_BT_PROVISION
            /* Never SYSTEM_OFF mid-radio-provision: a •• power-off during the ~minutes DS
             * write would kill the module mid-flash. The blocking flash already stalls this
             * whole scan, so this is belt-and-suspenders + refactor-proof. */
            if (bt_provision_is_active()) {
                held = 0;
            } else
#endif
            if (++held >= 625) {
                enter_bootloader();   /* never returns (SYSTEM_OFF) */
            }
        } else {
            held = 0;
        }
        int func_down = (func_low && func_armed && held < 625);

        /* If the active profile changed this tick (via the •• tap above OR a
         * host setactive/write/reset serviced by config_cdc_poll()), re-arm
         * every fader so the new profile's mapping re-emits at the faders'
         * current physical positions on the next read — otherwise the unchanged
         * raw codes would stay suppressed and the new CCs would never reach the
         * host/OP-XY until each fader was physically moved. */
        uint8_t now_active = clock_active_slot();   /* mode-aware (0..15), catches a MODE flip */
        if (now_active != last_active) {
            chord_flush_all();   /* v7: release held chords on ANY profile change (dial/host/switch) */
            faders_rearm();
            /* New profile = different CCs: drop soft-takeover memory so each bank
             * re-seeds and emits immediately (the prior jump-on-switch behavior),
             * not catch against another profile's stale values. */
            for (int i = 0; i < NUM_FADERS; i++) {
                g_takeover[i].pending = 0;
                g_takeover_arm[i] = 0;
                layer_takeover_clear_fader(&g_layer_takeover, i);
            }
            /* Clock: re-apply the newly-active profile's clock config. is_boot=false
             * loads THIS profile's default tempo (each profile can host clock at its
             * own tempo); a disabled/clock-less profile silences the clock. This
             * detector covers the dial + host set-active + ••+Vol paths; the ••+T4
             * MODE flip is handled in its own combo case (which resyncs last_active). */
            clock_apply_active(false);
            last_active = now_active;
            last_clk = clock_active_cfg();
        } else if (clock_active_cfg() != last_clk) {
            /* Same profile, but its clock config just changed under us - a LIVE
             * configurator edit synced over the config protocol. Re-apply just the
             * clock (no chord-flush / fader-rearm; those are for a real switch). */
            clock_apply_active(false);
            last_clk = clock_active_cfg();
        }

        /* Scan both ladders FIRST (buttons before faders, like the verified
         * looper). Each edge -> the mapping engine -> TRS MIDI; one scan can emit
         * up to 4 edges (a release + a press on each ladder).
         * LAYER CYCLE (test trigger, FELDD_SHIFT_TRIGGER_PLAY): the active layer
         * (0..3) is driven by the PLAY layer count-dial (#61 gesture pass: 1 tap =
         * +1, 2/3/4 taps = jump to layer 2/3/4 — replaces the old double-tap). PLAY
         * is reserved for this — its presses feed the gesture/dial detectors and it
         * never reaches map_button, so it can't double-fire MIDI (it does still
         * report to the live monitor). When the layer changes, the faders arm
         * soft-takeover so the new bank holds its last value until the fader
         * crosses it. With FELDD_SHIFT_TRIGGER_PLAY=0, PLAY maps normally and
         * layer_now is a constant 0 (stock behaviour). */
        struct button_event evt[4];
        int ne = buttons_scan(evt, 4);
        /* PLAY-held = the mode-gesture modifier (phase 2b moved off •• onto PLAY, spec
         * §5). PLAY is tracks-ladder idx 0, so its COMMITTED/debounced hold reads as
         * buttons_track_committed()==0 (the tracks ladder reports one button at a time;
         * 0 = PLAY held, 1..4 = a Track button, -1 = none). Using the committed state
         * (not the raw press) means a momentary glitch can't spuriously flip the mode. */
#ifdef CONFIG_FELDD_BT_LINK
        /* PLAY-held is now used ONLY by the •• + play BT gesture (idx 0 on the tracks
         * ladder). PLAY no longer shifts layers or re-arms faders, so it lives inside
         * the BT guard — the shipped no-BT build doesn't compute it at all. */
        int play_held = (buttons_track_committed() == 0);
        /* Feed the DEBOUNCED/armed •• level (func_down), not the raw func_low, so this
         * detector agrees with the idx-0 combo arm below (which also gates on func_down).
         * A single-scan •• glitch during a committed PLAY hold no longer starts/ends a
         * co-hold here and can't fire a spurious BT TOGGLE. func_down is func_low gated on
         * func_armed + held<625; during an active co-hold `held` is frozen to 0 (above), so
         * func_down tracks the physical •• hold for the whole gesture. */
        int bt_ev = bt_gesture_step(&bt_gest, func_down, play_held);
#ifndef CONFIG_FELDD_BT_LINK_HID
        /* PHASE-1 DEAD-BAND FIX: with no HID there are no bonds to forget, so an over-long (>= ~2.4 s)
         * •• + play hold returns BT_GESTURE_FORGET — which Phase 1 would otherwise DROP, giving no
         * toggle and no LED feedback (a UX dead band). Remap it to a plain TOGGLE so a long hold still
         * switches BT power. Phase 2 (CONFIG_FELDD_BT_LINK_HID, Task 12A) compiles this out and wires
         * the real fill-to-confirm forget instead. */
        if (bt_ev == BT_GESTURE_FORGET) { bt_ev = BT_GESTURE_TOGGLE; }
#endif
#ifdef CONFIG_FELDD_BT_LINK_HID
        /* Task 12A (a): while •• + play is held, show the progressive L->R fill-to-confirm. The LED
         * player below reads bt_gesture_fill() each tick (BT_LED_FORGET_FILL is progress-, not time-
         * driven), so the fill tracks the hold and doubles as the abort cue: release before full = the
         * SHORT toggle (BT_GESTURE_TOGGLE, handled just below), hold to full = FORGET (handled after). */
        if (bt_gesture_active(&bt_gest) && bt_anim_pat != BT_LED_FORGET_DONE) {
            bt_anim_pat = BT_LED_FORGET_FILL; bt_anim_tick = 0;
        } else if (bt_anim_pat == BT_LED_FORGET_FILL && !bt_gesture_active(&bt_gest)) {
            bt_anim_pat = BT_LED_NONE;   /* released before full: fill ends; the toggle below plays */
        }
#endif
        if (bt_ev == BT_GESTURE_TOGGLE) {
            if (!bt_link_bt_is_on()) {
                /* off->on: cold-boot the module + await READY. NOTE: bt_link_bt_on() BLOCKS the control
                 * loop up to ~1.5 s (WDT-fed poll for READY), so buttons/LEDs freeze for that window.
                 * Acceptable — this is a deliberate user gesture, and the momentary ON LED sweep queued
                 * below plays immediately after to confirm the power-on. */
                int rc = bt_link_bt_on();
                bt_anim_pat = (rc == 0) ? BT_LED_ON : BT_LED_UNAVAIL;   /* came up vs booted-silent */
            } else {
                bt_link_bt_off();                               /* on->off: hold module in reset */
                bt_anim_pat = BT_LED_OFF;
            }
            bt_anim_tick = 0;
            bt_midi_ready_was = 0;                              /* re-arm the connected edge */
        }
#ifdef CONFIG_FELDD_BT_LINK_HID
        /* Task 12A (b): full-hold forget commit. In Phase-1 builds BT_GESTURE_FORGET was remapped to
         * TOGGLE above (the #ifndef CONFIG_FELDD_BT_LINK_HID block) so it never reaches here; only the
         * HID build, where bonds actually exist, forgets for real. The remap and this branch are
         * mutually exclusive by construction. */
        else if (bt_ev == BT_GESTURE_FORGET) {
            bt_link_clear_bonds();                             /* FELDD_CMD_CLEAR_BONDS -> module forget + re-adv */
            bt_anim_pat = BT_LED_FORGET_DONE; bt_anim_tick = 0;   /* 3x blink commit (front-row on/off only) */
        }
#endif
#endif
        /* Device mode for THIS tick's routing + the mode-LED render below. Read the
         * RAM hot copy once; a flip in this scan updates it locally so the same-tick
         * render + any later edge already see the new mode. */
        int mode_now = librarian_mode();
        /* PLAY is a PLAIN assignable button (chord/note/CC per the profile). Since 0.24
         * (f821ae1) librarian_play_mode() is hardcoded to 1 = always-assignable, so PLAY
         * NEVER shifts the routing layer: layer_now is ALWAYS the ••+FWD/RWD engaged
         * layer. This retires the dormant play_mode==0 path (effective_layer + the
         * soft-takeover re-arm) that used to force-re-emit all four faders at the
         * PLAY-sagged rail value on every PLAY press/release edge (petercolombo fader
         * jitter, 2026-07-12). The side-row LED still FOLLOWS layer_now (0.22 Feature 2). */
        int layer_now = gesture_layer(&shift_gesture);

        for (int i = 0; i < ne; i++) {
            uint8_t idx = evt[i].idx;
            int pressed = evt[i].pressed;

#ifdef CONFIG_FELDD_BT_LINK
            /* •• + play (BT gesture) reuses the combo engine's consume-both-edges +
             * reset_hold on PLAY (idx 0). combo_dispatch keeps PLAY OUT of range by
             * design (mode.c; host-tested test_mode.c pins "PLAY out of range"), so the
             * idx-0 arm lives HERE behind CONFIG_FELDD_BT_LINK — the shipped build never
             * consumes PLAY as a combo. It mirrors the engine using g_combo's otherwise-
             * unused bit 0 (combo_dispatch only ever touches idx 1..8): a PLAY press while
             * •• is the Fn modifier (func_down) is consumed and voids the •• power-off
             * hold (reset_hold), and the matching release is swallowed off the latch on
             * either crossing order. The BT action itself (toggle vs forget, short vs
             * long) is classified by bt_gesture_step() above; this arm only suppresses
             * PLAY's shift-emit + the power-off, replacing the retired func_combo /
             * play_combo_consumed freeze that once guarded the deleted PLAY peek FSM. A
             * plain PLAY (•• not down) is NOT consumed here — it falls through to the
             * combo_dispatch passthrough + the play_mode shift handling below. */
            if (idx == 0) {
                const uint16_t play_bit = 1u;   /* g_combo bit 0; combo_dispatch uses idx 1..8 only */
                if (pressed && func_down) {
                    g_combo.latched |= play_bit;
                    held = 0;                              /* reset_hold: void the •• power-off */
                    config_cdc_monitor_button(0, pressed);
                    continue;                              /* consumed: no shift-emit */
                }
                if (!pressed && (g_combo.latched & play_bit)) {
                    g_combo.latched &= (uint16_t)~play_bit;
                    config_cdc_monitor_button(0, pressed);
                    continue;                              /* release swallowed off the latch */
                }
            }
#endif

            /* dot-dot (••) Fn-modifier combo (spec (a),(b)2): while •• is held, T4
             * flips MODE, Vol+/- cycle the profile, FWD/RWD step the layer. The
             * press is latched per index and the matching RELEASE swallowed off the
             * latch (mode.c combo_dispatch, host-tested test_mode.c), regardless of
             * the instantaneous •• level at the release edge (spec (h)1). Any
             * consumed press resets `held` so the •• power-off hold is voided
             * (clean-hold gate, spec (b)4). The mode flip is a genuine single-fire
             * toggle (mode_toggle) so Keyboard stays reachable and cannot double-fire
             * (spec (b)5). This retires mode_decide + the mode.c gesture template. */
            struct combo_decision cd = combo_dispatch(&g_combo, func_down, idx, pressed);
            if (cd.reset_hold) {
                held = 0;
            }
            if (cd.consumed) {
                if (pressed) {
                    switch (cd.action) {
                    case COMBO_MODE_TOGGLE: {
                        uint8_t next = mode_toggle(librarian_mode());
                        if (librarian_set_mode(next) == 0) {
                            mode_now = next;
                            /* End the Keyboard session on a flip: drop held keys +
                             * send the all-zero key-up so nothing sticks; release
                             * held chords; arm the NON-BLOCKING side-row MODE-FLASH;
                             * push the new mode + the bank's active; re-arm faders for
                             * the new bank's CCs + clear the soft-takeover memory. */
                            kbd_state_reset(&g_kbd);
                            kbd_send_held();
                            chord_flush_all();
                            mode_flash_mode  = next;
                            mode_flash_ticks = MODE_FLASH_TICKS;
                            config_cdc_monitor_mode(next);
                            config_cdc_monitor_active(librarian_active_index());
                            faders_rearm();
                            for (int fi = 0; fi < NUM_FADERS; fi++) {
                                g_takeover[fi].pending = 0;
                                g_takeover_arm[fi] = 0;
                                layer_takeover_clear_fader(&g_layer_takeover, fi);
                            }
                            /* Clock: the mode flip moved to the OTHER bank's profile
                             * (its own clock_cfg). Re-apply here + resync last_active to
                             * the new mode-aware slot so the detector does not also fire. */
                            clock_apply_active(false);
                            last_active = clock_active_slot();
                            last_clk = clock_active_cfg();
                        }
                        break;
                    }
                    case COMBO_PROFILE_NEXT:
                        /* ••+Vol+ : next profile WITHIN the bank (wraps 7->0). The
                         * now_active!=last_active block above re-arms the faders on
                         * the next tick. */
                        if (librarian_set_active(lib_bank_cycle(librarian_active_index())) == 0) {
                            config_cdc_monitor_active(librarian_active_index());
                        }
                        break;
                    case COMBO_PROFILE_PREV:
                        /* ••+Vol- : previous profile within the bank (wraps 0->7). */
                        if (librarian_set_active(lib_bank_cycle_prev(librarian_active_index())) == 0) {
                            config_cdc_monitor_active(librarian_active_index());
                        }
                        break;
                    case COMBO_LAYER_NEXT:
                        /* ••+FWD : next layer (relative +1, wrap at 8). */
                        gesture_set_layer(&shift_gesture,
                            gesture_layer_step(gesture_layer(&shift_gesture), +1));
                        chord_flush_all();
                        for (int fi = 0; fi < NUM_FADERS; fi++) {
                            g_takeover_arm[fi] = 1;
                        }
                        break;
                    case COMBO_LAYER_PREV:
                        /* ••+RWD : previous layer (relative -1, wrap at 8). */
                        gesture_set_layer(&shift_gesture,
                            gesture_layer_step(gesture_layer(&shift_gesture), -1));
                        chord_flush_all();
                        for (int fi = 0; fi < NUM_FADERS; fi++) {
                            g_takeover_arm[fi] = 1;
                        }
                        break;
                    case COMBO_BRIGHTNESS: {
                        /* Feature B (0.23): ••+T2 toggles the persisted LED brightness
                         * (0 dim <-> 1 full) and applies it live. */
                        uint8_t nb = librarian_brightness() ? 0u : 1u;
                        librarian_set_brightness(nb);
                        led_set_brightness(nb ? LED_BRIGHTNESS_FULL : LED_BRIGHTNESS_DEFAULT);
                        break;
                    }
                    case COMBO_PANIC: {
                        /* Feature B (0.23): ••+T3 sends the MIDI panic set (CC
                         * 120/121/123 on all 16 channels) and arms the confirm flash. */
                        struct midi_msg pm[48];
                        int pn = panic_fill(pm, 48);
                        for (int i = 0; i < pn; i++) {
                            midi_out_send(&pm[i], NULL);
                        }
                        g_panic_flash_ticks = PANIC_FLASH_TICKS;
                        break;
                    }
                    case COMBO_BATTERY:
                        /* Feature B (0.23): ••+T1 is a HELD display, rendered in the
                         * LED block below, not an edge action. */
                        break;
                    default:
                        break;
                    }
                }
                config_cdc_monitor_button(idx, pressed);   /* still report the swallowed edge */
                continue;
            }

            /* Tap-tempo: when the active profile's clock is on AND this button is its
             * assigned tap button, a press taps the tempo. Placed after the •• combo
             * consume above, so ••+<button> combos are untouched. The button is
             * SWALLOWED (tempo only, no MIDI) - it is a tap pad on this profile. Feed
             * clock_tap_bpm a ms-resolution timestamp (ample for human tapping). */
            if (clock_router_enabled() && (int)clock_router_tap_button() == idx) {
                if (pressed) {
                    uint16_t bpm = clock_tap_bpm(&g_clock_tap,
                                        (uint32_t)(k_uptime_get() * 1000));
                    if (bpm) {
                        clock_set_live_bpm(bpm);
                    }
                }
                config_cdc_monitor_button(idx, pressed);
                continue;
            }

            /* PLAY (idx 0) is a PLAIN assignable button (librarian_play_mode() is
             * hardcoded to 1 since 0.24). It falls straight through to normal routing;
             * make_default seeds button[0] = BTN_NONE (silent) until the user maps it,
             * so there is no CC#0/fader0 collision (spec (c)). The dormant play_mode==0
             * momentary-shift swallow is retired here. */

            /* Normal routing on the EFFECTIVE layer. KEYBOARD holds the per-button
             * HID key while pressed (kbd_state latches (mod,key) at the press edge,
             * so a springback can't strand it). MIDI routes through the stateful
             * dispatcher; the emit layer is LATCHED at PRESS and reused on RELEASE
             * (stuck-note-across-springback, spec (h)2) so a NOTE pressed on shift-L2
             * releases its Note-Off on L2 even after PLAY springs back. Faders are
             * ALWAYS MIDI (handled below on layer_now). */
            if (mode_now == MODE_KEYBOARD) {
                if (pressed) {
                    const struct profile *p = librarian_active();
                    g_btn_press_layer[idx] = (uint8_t)layer_now;
                    kbd_state_press(&g_kbd, idx,
                        profile_layer_button_mod(p, idx, layer_now),
                        profile_layer_button_key(p, idx, layer_now));
                } else {
                    kbd_state_release(&g_kbd, idx);
                }
                kbd_send_held();
            } else {
                int emit_layer = layer_now;
                if (pressed) {
                    g_btn_press_layer[idx] = (uint8_t)layer_now;
                } else {
                    emit_layer = g_btn_press_layer[idx];
                }
                (void)route_midi_button(idx, pressed, emit_layer);
            }
            config_cdc_monitor_button(idx, pressed);   /* mon frame */
        }

        /* Track1+4 held the full ~1.2 s -> reset into the bootloader for DFU. */
        if (buttons_dfu_held()) {
            enter_dfu();
        }

        /* TRACK-row (FRONT) render — strict per-tick priority, re-derived every
         * tick (0.9.1 LED redesign, spec §4; Feature 4 retargets the peek; 0.23 adds
         * the •• utility row on top). The front row is the func-mode PROFILE PEEK +
         * plain PRESS-FEEDBACK surface; the LAYER lives on the SIDE row below.
         * Highest priority wins:
         *   0. PANIC confirm flash (0.23) - while g_panic_flash_ticks > 0, light ALL
         *      track + side LEDs, a brief transient acknowledging ••+T3.
         *   1. ••+T1 BATTERY peek (0.23) - while •• is held and Track 1 is committed,
         *      show the battery gauge on the SIDE row (below) and keep the track row
         *      dark, so the profile peek does not also draw.
         *   2. DFU all-4-solid  - handled by enter_dfu() above (never returns).
         *   3. FUNC-mode PROFILE PEEK - while •• is held (func_down), show the
         *      current within-bank profile LIVE (dial_profile_peek_pattern renders
         *      active_index+1: 1-4 solid, 5-8 solid+blink). The old •• burst dial +
         *      confirm-flash peek path is GONE (both count-dials retired, Feature 4).
         *   4. PRESS feedback (PLAIN) - when a Track button is committed, light ONLY
         *      that button's own front LED (on = i == pressed_idx). The ladder reads
         *      one button at a time -> at most one Track held (spec §4).
         *   5. ALL-DARK at rest - the layer lives on the side row now, so the front
         *      row is the intended calm idle (all off) when nothing is pressed. */
        const uint32_t track_leds[4] = {
            SP1_TRACK_LED1, SP1_TRACK_LED2, SP1_TRACK_LED3, SP1_TRACK_LED4
        };
        /* 0.23 •• utility-row peeks, captured once so the side-row render below agrees
         * with this block within the tick. ••+T1 (Track 1 committed) = battery peek. */
        int panic_active = (g_panic_flash_ticks > 0);
        int batt_peek    = (func_down && buttons_track_committed() == 1);

        if (panic_active) {
            /* Priority 0: MIDI-panic confirm flash - ALL track + side LEDs on. The
             * side-row render below skips while panic_active so this is not overdrawn. */
            for (int i = 0; i < 8; i++) {
                led_idx(i, true);
            }
            g_panic_flash_ticks--;
        } else if (batt_peek) {
            /* Priority 1: ••+T1 battery peek. The gauge draws on the SIDE row (below);
             * keep the track row dark so the profile peek does not also draw. */
            for (int i = 0; i < 4; i++) {
                led_pin(track_leds[i], false);
            }
        } else if (func_down) {
            /* Priority 3: func-mode profile peek owns the whole track row. Single
             * decode point shared with the host test (dial_profile_peek_pattern);
             * live scan tick drives the 5-8 blink phase. */
            unsigned char out[4];
            dial_profile_peek_pattern((unsigned char)librarian_active_index(),
                                      (unsigned char)tick, out);
            for (int i = 0; i < 4; i++) {
                led_pin(track_leds[i], out[i] ? true : false);
            }
        } else {
            /* Priority 3 (PLAIN press feedback) + priority 4 (all-dark rest). When a
             * Track button is committed, light ONLY its own front LED; when nothing
             * is held the row is all-dark (the layer LED lives on the side row now).
             * The ladder reads one button at a time, so there is never a two-button
             * case. */
            int trk_held = buttons_track_committed();        /* 1..4 = a Track, else none */
            int pressed_idx = (trk_held >= 1 && trk_held <= 4) ? (trk_held - 1) : -1;
            for (int i = 0; i < 4; i++) {
                int on = (i == pressed_idx);                 /* plain: only the pressed LED */
                if (on) {
                    led_pin(track_leds[i], true);
                } else {
                    led_pin(track_leds[i], false);
                }
            }
        }

        /* SIDE-row (PLAY-LED) render — strict per-tick priority, re-derived every
         * tick (0.9.1 LED redesign, spec §3). The side row is now the LAYER +
         * MODE-FLASH surface; the old STATIC mode pattern is RETIRED. Two writers
         * arbitrated by a single non-blocking deadline (mode_flash_ticks), priced
         * exactly like the proven front-row dial_confirm_ticks countdown:
         *   1. MODE-FLASH (transient) — while mode_flash_ticks > 0, flash the
         *      captured NEW mode's pattern (PULSE solid / BLINK per MODE_FLASH_STYLE).
         *   2. LAYER rest (permanent) — one side LED lit at position (layer & 3):
         *      SOLID for layers 0..3, BLINK on (tick/12)&1 for layers 4..7
         *      (gesture_layer(), natural ordering: layer 0 -> SP1_PLAY_LED1). The
         *      layer is unchanged across a mode switch, so the flash yields back to
         *      the same layer LED. Never all-dark for layers 0..3; layers 4..7 are
         *      dark on the blink off-phase (~96 ms).
         * The pure side_led_pattern() helper (side_led.c, host-tested) owns the
         * render math; main.c is the GPIO arbiter + the deadline countdown. Under
         * FELDD_SHIFT_TRIGGER_PLAY=0 there is no PLAY gesture FSM, so the layer pins
         * to 0 (SP1_PLAY_LED1 = the single resting layer), mirroring the old
         * front-row guard. */
        /* Side row FOLLOWS the effective layer (layer_now): a held PLAY shift moves
         * the dot to the profile's shift target and releasing snaps it back. This
         * DELIBERATELY reverses the old "spec (d)" (a momentary shift must not move
         * the LED); the solid/blink encoding already disambiguates same-column
         * layers (e.g. L2 solid vs L6 blink), so no new cadence is needed.
         * (0.22 Feature 2, spec 2026-07-08-feldd-022.) */
        unsigned char side_layer = (unsigned char)layer_now;
        if (panic_active) {
            /* Side LEDs are already lit by the panic confirm flash above; do not
             * overdraw them this tick (they stay on for the flash window). */
        } else if (batt_peek) {
            /* 0.23 ••+T1 battery peek: draw the stock-style charge gauge on the side
             * row (the same call the charge-standby gate uses); clears on release when
             * the normal layer render resumes. */
            charge_gauge(battery_pct(controls_read_raw(6)), charging(), tick);
        } else {
            unsigned char side_out[4];
            side_led_pattern(side_layer, (unsigned int)mode_flash_ticks,
                             mode_flash_mode, (MODE_FLASH_STYLE == 1),
                             (unsigned char)tick, side_out);
#ifdef FELDD_MODE_LED_SINGLE
            /* Fallback (build with -DFELDD_MODE_LED_SINGLE): collapse the whole side
             * indicator onto the fully-validated P1.13 (SP1_LED1 = SP1_PLAY_LED4)
             * and NEVER drive the other 3 side pins. The OR over side_out reproduces
             * the helper's intent on one pin: LAYER rest -> P1.13 solid (the layer
             * dot), MODE-FLASH -> P1.13 pulses/blinks with the flash window. A single-LED
             * build-time option (spec §2.1). */
            if (side_out[0] || side_out[1] || side_out[2] || side_out[3]) {
                led_pin(SP1_LED1, true);
            } else {
                led_pin(SP1_LED1, false);
            }
#else
            const uint32_t play_leds[4] = {
                SP1_PLAY_LED1, SP1_PLAY_LED2, SP1_PLAY_LED3, SP1_PLAY_LED4
            };
            for (int i = 0; i < 4; i++) {
                if (side_out[i]) {
                    led_pin(play_leds[i], true);
                } else {
                    led_pin(play_leds[i], false);
                }
            }
#endif
            /* Non-blocking countdown: when it hits 0 the next tick re-asserts the
             * layer LED automatically (no explicit clear), exactly like
             * dial_confirm_ticks. */
            if (mode_flash_ticks > 0) {
                mode_flash_ticks--;
            }
        }

#ifdef CONFIG_FELDD_BT_LINK
        /* Connected (host subscribed) rising edge -> double-flash, unless an on/off/unavail
         * animation is already playing (don't stomp it). */
        {
            int ready = bt_link_midi_ready() ? 1 : 0;
            if (ready && !bt_midi_ready_was && bt_anim_pat == BT_LED_NONE) {
                bt_anim_pat = BT_LED_CONNECTED; bt_anim_tick = 0;
            }
            bt_midi_ready_was = ready;
        }
        /* Momentary player: while active it OWNS the 4 FRONT track LEDs (idx 0-3) ONLY, overriding
         * this tick's normal track render, then hands back (design: momentary). FRONT-ROW ON/OFF
         * ONLY — it deliberately does NOT call led_set_brightness(): that control is GLOBAL (led.h,
         * all 8 LEDs), so modulating it here would momentarily disturb the side/play LEDs (idx 4-7),
         * which are meaningful (project_feldd_led_status_language). The frame's on/off bits render at
         * the ambient global brightness; the brightness bt_led returns is advisory and intentionally
         * NOT applied. (Runs BEFORE the host-LED console override below, which stays the highest-
         * priority writer when a host owns the LEDs.) */
        if (bt_anim_pat != BT_LED_NONE) {
            uint8_t f[4];
            (void)bt_led_pattern_frame(bt_anim_pat, bt_anim_tick, bt_gesture_fill(&bt_gest), f);
            for (int i = 0; i < 4; i++) { led_idx(i, f[i]); }   /* FRONT row only; side row untouched */
            if (++bt_anim_tick >= bt_led_pattern_len(bt_anim_pat)) {
                bt_anim_pat = BT_LED_NONE;                      /* next tick normal render resumes */
            }
        }
#endif

        /* Faders -> CC, AFTER the buttons. A held button sags the shared BTN_COM
         * rail, and because the SAADC ref is internal (not ratiometric) every fader
         * ADC read drops ~1-2 LSB. We gate on buttons_rail_loaded() — the
         * INSTANTANEOUS raw ladder read captured by buttons_scan() above — NOT the
         * 3-read committed state (which lags the press ~24 ms), so the very first
         * drooped ticks don't leak spurious CC dips (the v0.5.2 fix; petercolombo's
         * bench report, 2026-06-18).
         *
         * 0.15 (BUG-5, petercolombo): the skip now EDGE-triggers on the press only
         * (fader_settle_step re-arms on the rising rail edge, then counts down even
         * while the button stays held) instead of pinning the skip for the whole
         * hold. A SUSTAINED hold no longer freezes the faders, so riding the faders
         * while holding a chord button tracks live and there is no snap-to-hardware
         * on release. During the hold we still read every tick but pass rail_loaded
         * to fader_update_rail, which widens the droop deadband across the rail
         * bands so the persistent sag is swallowed while genuine moves pass. */
        static uint8_t fader_settle;
        static int     fader_rail_prev;
        int rail_loaded = buttons_rail_loaded();
        {   /* Close the intra-tick press race: buttons_scan() sampled the ladders at the
             * TOP of this tick, but a press that landed since then sags the rail unseen,
             * so the worst transient would otherwise hit the fader read on the un-widened
             * idle path (incl. the 0/127 rail-band bypass). Re-probe NOW and take the
             * heavier class. (petercolombo jitter, 2026-07-12.) */
            int probe = buttons_rail_probe();
            if (probe > rail_loaded) rail_loaded = probe;
        }
        /* fader_settle_step edge-detects the rising rail load; feed it booleans so the
         * new 0/1/2 class collapses to loaded/idle exactly as before. */
        fader_settle = fader_settle_step(rail_loaded != 0, fader_rail_prev != 0, fader_settle);
        fader_rail_prev = rail_loaded;
        if (fader_settle == 0) {
            for (int idx = 0; idx < NUM_FADERS; idx++) {
                int raw = controls_read_raw(2 + idx);
                if (raw < 0) {
                    continue;   /* read error this tick; try again next loop */
                }
                /* 0.26: undo the BTN_COM rail droop a held button induces on this fader
                 * (proportional to position + the button's rail class) so a button press
                 * never dips the CC, while fine 1-CC moves still track (petercolombo). */
                uint16_t raw_c = fader_rail_compensate((uint16_t)raw, rail_loaded);
                uint8_t phys_cc = fader_raw_to_cc(raw_c);

                /* Soft-takeover: a pending arm from a shift toggle sets up the
                 * catch against the new bank's last value (or seeds if this bank
                 * was never sent in this profile). initialized=0 forces a clean
                 * re-emit the moment the catch releases. */
                if (g_takeover_arm[idx]) {
                    g_takeover_arm[idx] = 0;
                    if (layer_takeover_valid(&g_layer_takeover, idx, layer_now)) {
                        takeover_arm(&g_takeover[idx], phys_cc,
                                     layer_takeover_last(&g_layer_takeover, idx, layer_now));
                    } else {
                        g_takeover[idx].pending = 0;   /* seed: nothing to catch */
                    }
                    g_fader[idx].initialized = 0;
                }
                /* While catching, suppress emits until the fader crosses the held
                 * value; the host keeps showing that value, so no jump. */
                if (!takeover_step(&g_takeover[idx], phys_cc)) {
                    continue;
                }

                int cc = fader_update_rail(&g_fader[idx], raw_c, rail_loaded);
                if (cc >= 0) {
                    /* BPM fader: when the active profile's clock is on and this is its
                     * assigned BPM fader, the fader sets the TEMPO instead of sending
                     * CC (tempo-only, so the clock byte never fights a CC on the shared
                     * TRS wire). fader_update_rail returns cc>=0 only on a real move, so
                     * a held fader never flickers the tempo. */
                    if (clock_router_enabled() && (int)clock_router_bpm_fader() == idx) {
                        clock_set_live_bpm(clockgen_fader_bpm((uint8_t)cc, 40, 240));
                        config_cdc_monitor_fader(idx, cc);   /* still report the move */
                    } else {
                        map_fader(librarian_active(), idx, cc, layer_now, midi_out_send, NULL);
                        config_cdc_monitor_fader(idx, cc);   /* mon frame if monitoring */
                        layer_takeover_record(&g_layer_takeover, idx, layer_now, (uint8_t)cc);
                        /* Stage 2: cache the settled CC of a chord_depth fader so the
                         * next chord press samples this layer's depth band. The scan is
                         * idx 0..3 ascending, so the highest-idx chord_depth fader is the
                         * last writer (the firmware fallback to the UI one-per-layer rule). */
                        if (profile_layer_fader_role(librarian_active(), idx, layer_now)
                                == FADER_ROLE_CHORD_DEPTH) {
                            g_chord_depth_cc[layer_now] = cc;   /* cache the settled CC */
                        }
                    }
                }
            }
        }

        /* (0.9.1 LED redesign: the side row shows the LAYER permanently (one LED at
         * gesture_layer()) + a temporary MODE-FLASH on a switch — rendered above by
         * side_led_pattern(). The default build drives the 4 side play LEDs; the
         * -DFELDD_MODE_LED_SINGLE fallback collapses the indicator onto SP1_LED1/
         * P1.13 only. The front/track row is press-feedback + dial/peek, all-dark
         * at rest.) */

        /* feldd-cc console: the host-LED override is the HIGHEST-PRIORITY writer.
         * It runs LAST in the tick, after the normal track + side render above, so
         * when a host owns the LEDs (via the `led` CDC verb) all 8 are driven from
         * its mask and whatever the normal render wrote this tick is overwritten.
         * When inactive this is a pure no-op, so the normal render is byte-for-byte
         * unchanged. bit i -> LED i: 0-3 = track row, 4-7 = play/side row. Drives
         * ONLY the existing LED pins (introduces no new GPIO). */
        if (host_led_active()) {
            uint8_t hm = host_led_get_mask();
            const uint32_t hl_track[4] = {
                SP1_TRACK_LED1, SP1_TRACK_LED2, SP1_TRACK_LED3, SP1_TRACK_LED4
            };
            for (int i = 0; i < 4; i++)
                led_pin(hl_track[i], ((hm >> i) & 1u) != 0u);
#ifdef FELDD_MODE_LED_SINGLE
            /* the other 3 side pins are never driven in this build: fold the 4 side
             * bits onto the one validated side LED (P1.13). */
            if (hm & 0xF0u) led_pin(SP1_LED1, true);
            else            led_pin(SP1_LED1, false);
#else
            const uint32_t hl_play[4] = {
                SP1_PLAY_LED1, SP1_PLAY_LED2, SP1_PLAY_LED3, SP1_PLAY_LED4
            };
            for (int i = 0; i < 4; i++)
                led_pin(hl_play[i], ((hm >> (i + 4)) & 1u) != 0u);
#endif
        }

        /* Clock GEN/THRU: age out a vanished USB-in master (>2 s) and resume the
         * internal generator at its last measured tempo. External clock bytes are
         * fed in from the USB class OUT callback (clock_router_ext_rt); this only
         * handles the timeout/fallback side, so it runs every tick. */
        clock_router_service();

        /* Debounced global-BPM persistence: write the settled tempo to NVS once the
         * BPM fader/tap has stopped moving for ~3 s, so a sweep does not wear flash.
         * librarian_set_bpm no-ops when the value is unchanged. */
        if (g_bpm_save_at && k_uptime_get() >= g_bpm_save_at) {
            g_bpm_save_at = 0;
            librarian_set_bpm(g_bpm_pending);
        }

        tick++;
        k_msleep(8);
    }
    return 0;
}
