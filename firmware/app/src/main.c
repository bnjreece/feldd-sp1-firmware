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
#include "midi_out.h"
#include "usbdev.h"
#include "usb_hid.h"
#include "librarian.h"
#include "lib_bank.h"
#include "config_cdc.h"
#include "gesture.h"
#include "dial.h"
#include "mode.h"
#include "side_led.h"
#include "wdt.h"

/* TEMPORARY shift-layer test trigger (set to 0 to restore stock behaviour).
 * When 1: double-tapping PLAY (button index 0) latches the shift bank on/off,
 * and PLAY does nothing else (its normal mapping is never emitted). This keeps
 * the shift trigger off the overloaded •• button so it can be validated on the
 * bench without colliding with •• tap(next-profile)/hold(power-off). It is
 * expected to move to its own gesture after testing. See gesture.c. */
#ifndef FELDD_SHIFT_TRIGGER_PLAY
#define FELDD_SHIFT_TRIGGER_PLAY 1
#endif

/* M5.2: the active profile that drives the mapping engine now comes from the
 * NVS-backed librarian (librarian.c), not a hardcoded g_default. The librarian
 * owns the 8 default profiles and the persisted active index; the control loop
 * reads librarian_active() (a RAM hot copy) every tick. */

/* Per-fader send-on-change state for fader_update (zero-init = un-initialized,
 * the first read seeds last_sent_raw and emits). */
static fader_t g_fader[NUM_FADERS];

/* Soft-takeover ("pickup") for SHIFT-layer switches. Per fader we remember the
 * last CC each of the 2 banks (0=base, 1=shift) emitted, so on a shift toggle the
 * new bank holds its previous value until the physical fader CROSSES it, then
 * resumes normal snappy tracking - no value jump. g_bank_valid gates the
 * first-ever emit on a bank (seed). g_takeover_arm defers the catch setup to the
 * next fader read (which has a fresh position). Profile switches DO jump (they
 * re-arm + clear this), matching the prior behavior. */
static takeover_t g_takeover[NUM_FADERS];
static uint8_t    g_bank_last_cc[NUM_FADERS][2];
static uint8_t    g_bank_valid[NUM_FADERS][2];
static uint8_t    g_takeover_arm[NUM_FADERS];

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
    /* F6: SYSTEM_OFF on nRF52840 only wakes on a configured DETECT/sense, a
     * full reset, or USB. To return from power-off on a •• press we MUST arm a
     * sense-low wake source on the •• pin BEFORE entering off. We also park the
     * output pins (LEDs, BTN_COM rail, TRS ring) to cut leakage in the off
     * state. Order matters: teardown outputs, ARM the wake-sense pin, then clear
     * RESETREAS, then SYSTEMOFF. We never touch P0.27's config after arming it. */

    /* Park output pins low / disable the TRS ring. LEDs off (clear = off). */
    nrf_gpio_pin_clear(SP1_LED1);
    nrf_gpio_pin_clear(SP1_TRACK_LED1);
    nrf_gpio_pin_clear(SP1_TRACK_LED2);
    nrf_gpio_pin_clear(SP1_TRACK_LED3);
    nrf_gpio_pin_clear(SP1_TRACK_LED4);
    /* Park the 3 spare side (play) LEDs too (P0.01/P1.12/P0.00). Without this the
     * lit LAYER / mode-flash pattern (0.9.1) stays driven and is RETAINED through
     * SYSTEM_OFF, draining the cell (the 0.7.0-beta "LEDs never turn off" report).
     * SP1_PLAY_LED4 == SP1_LED1 is already cleared above. Skipped under
     * -DFELDD_MODE_LED_SINGLE where these 3 pins are never driven. */
#ifndef FELDD_MODE_LED_SINGLE
    nrf_gpio_pin_clear(SP1_PLAY_LED1);
    nrf_gpio_pin_clear(SP1_PLAY_LED2);
    nrf_gpio_pin_clear(SP1_PLAY_LED3);
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
     * a ~3 s hold). If we armed the level-sense now, the already-low pin would
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
    nrf_gpio_cfg_output(SP1_TRACK_LED1);
    nrf_gpio_cfg_output(SP1_TRACK_LED2);
    nrf_gpio_cfg_output(SP1_TRACK_LED3);
    nrf_gpio_cfg_output(SP1_TRACK_LED4);
    nrf_gpio_pin_set(SP1_TRACK_LED1);
    nrf_gpio_pin_set(SP1_TRACK_LED2);
    nrf_gpio_pin_set(SP1_TRACK_LED3);
    nrf_gpio_pin_set(SP1_TRACK_LED4);
    NRF_POWER->GPREGRET = 0x57u;
    __DSB();
    NVIC_SystemReset();
    for (;;) { }
}

/* Track-row profile/dial render (spec §8 + §4). The old profile_blink() was a
 * BLOCKING k_msleep chase (up to ~960 ms for an 8-count) that dropped MIDI/fader
 * edges while it slept — fatal once we need to COUNT •• taps (you can't count
 * while sleeping; spec §8 "Mandatory prerequisite"). It is now a per-tick render:
 * the control loop calls this every iteration while a dial burst is active OR a
 * post-commit CONFIRM flash is counting down, passes the live dial count + scan
 * tick, and dial_track_pattern() (the host-tested pure mapping) hands back the
 * 4-LED on/off state for THIS tick. Non-blocking, re-derived every tick, on the
 * FRONT/track row only — the SIDE row (layer + mode-flash, 0.9.1) is a separate
 * writer, so the two rows never collide (spec §8). */
static void profile_track_render(unsigned char count, unsigned char tick)
{
    const uint32_t leds[4] = {
        SP1_TRACK_LED1, SP1_TRACK_LED2, SP1_TRACK_LED3, SP1_TRACK_LED4
    };
    unsigned char out[4];
    dial_track_pattern(count, tick, out);
    for (int i = 0; i < 4; i++) {
        if (out[i]) {
            nrf_gpio_pin_set(leds[i]);
        } else {
            nrf_gpio_pin_clear(leds[i]);
        }
    }
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

/* CHARGE-STANDBY GATE (mirrors the chattock sp1-tape-looper). The SP-1
 * bootloader hands control to the app on ANY power event: a deliberate ••
 * power-on, but ALSO a bare USB-charge plug-in, a battery insert, OR the
 * soft-reset right after a flash. Only a •• wake (RESETREAS.OFF) or a watchdog
 * recovery (RESETREAS.DOG) is a "real" turn-on. For anything else we must NOT
 * spin up the full app (USB, SAADC, TRS) on what may be a nearly-flat cell -
 * that is exactly what brown-out-thrashes a low battery and can wedge the
 * device out of reach of the bootloader (the bug that bricked Unit A on the
 * bench). So park here: blink LED1 as a charge cue, wait for a ~0.6 s •• hold
 * to actually switch on, and on battery with nothing held drop to a clean
 * SYSTEM_OFF (a •• press wakes it). */
static void charge_standby_gate(uint32_t wake_reas)
{
    /* Real power-on (•• press) or watchdog recovery -> straight to full boot. */
    if (wake_reas & (POWER_RESETREAS_OFF_Msk | POWER_RESETREAS_DOG_Msk)) {
        return;
    }

    int64_t hold_t = -1;
    uint32_t blink = 0;
    for (;;) {
        feed_wdt();
        if (nrf_gpio_pin_read(SP1_FUNC_BTN) == 0) {   /* •• pressed */
            if (hold_t < 0) {
                hold_t = k_uptime_get();
            } else if (k_uptime_get() - hold_t >= 600) {
                break;                                /* held ~0.6 s -> power on */
            }
            nrf_gpio_pin_set(SP1_LED1);               /* press feedback */
        } else {                                      /* •• released */
            hold_t = -1;
            if (!usb_present()) {
                enter_bootloader();                   /* on battery + idle -> SYSTEM_OFF */
            }
            /* Charge cue: blink LED1 while charging, solid when full / done. */
            if (charging()) {
                ((++blink / 12u) & 1u) ? nrf_gpio_pin_set(SP1_LED1)
                                       : nrf_gpio_pin_clear(SP1_LED1);
            } else {
                nrf_gpio_pin_set(SP1_LED1);
            }
        }
        k_msleep(40);
    }

    nrf_gpio_pin_clear(SP1_LED1);
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
    const uint32_t leds[4] = {
        SP1_TRACK_LED1, SP1_TRACK_LED2, SP1_TRACK_LED3, SP1_TRACK_LED4
    };
    for (int i = 0; i < 4; i++) {
        nrf_gpio_cfg_output(leds[i]);
        nrf_gpio_pin_clear(leds[i]);
    }
    for (int pass = 0; pass < FELDD_BOOT_SIG_SWEEPS; pass++) {
        for (int i = 0; i < 4; i++) {
            feed_wdt();
            nrf_gpio_pin_set(leds[i]);
            k_msleep(80);
            nrf_gpio_pin_clear(leds[i]);
        }
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
    nrf_gpio_cfg_output(SP1_LED1);
    /* The 3 spare side (play) LEDs drive the LAYER indicator + mode-flash (0.9.1).
     * Configure them as outputs alongside SP1_LED1 (=P1.13, play LED 4). All four
     * are SYNTH-clock pins (NOT the LF crystal), already driven always-on by 0.9.0;
     * GPIO-only and Track1+4-DFU-recoverable. Skipped under -DFELDD_MODE_LED_SINGLE
     * (validated P1.13 only). */
#ifndef FELDD_MODE_LED_SINGLE
    nrf_gpio_cfg_output(SP1_PLAY_LED1);   /* P0.01 */
    nrf_gpio_cfg_output(SP1_PLAY_LED2);   /* P1.12 */
    nrf_gpio_cfg_output(SP1_PLAY_LED3);   /* P0.00 */
#endif
    nrf_gpio_cfg_input(SP1_FUNC_BTN, NRF_GPIO_PIN_PULLUP);

    charger_init();         /* enable battery charging ASAP so a low cell can't brown us out */
    /* Park in low-power charge-standby unless this was a deliberate •• power-on
     * or a watchdog recovery, so a full boot can never brown-out-thrash a low
     * cell (the failure that wedged Unit A). Returns only on a real turn-on. */
    charge_standby_gate(wake_reas);

    boot_signature();       /* feldd boot animation: track-LED 1->2->3->4 sweep */

    controls_init();        /* power the ladder/fader rail + set up SAADC channels */
    buttons_init();         /* reset the ladder decode/debounce state */
    midi_out_init();        /* bring up uart1 TRS MIDI out + enable the ring PNP */
    usbdev_start();         /* enumerate the USB composite: CDC console + USB-MIDI */

    /* M5.2: mount NVS, lay down 8 default profiles on first boot, and load the
     * persisted active profile into the librarian's RAM hot copy. If NVS fails
     * to come up (e.g. an unexpected flash fault), the control loop below still
     * runs against whatever librarian_active() returns — librarian_init zeroes
     * its state, so map_* sees a benign empty profile rather than crashing. */
    int lib_rc = librarian_init();
    printk("LIB init rc=%d active=%d\n", lib_rc, librarian_active_index());

    /* M6.2: bind the CDC console as the JSON-lines config protocol channel and
     * start its line reader. From here on the control loop calls
     * config_cdc_poll() each tick to service host requests and emits monitor
     * frames for fader CCs / button edges when monitoring is enabled. */
    int cdc_rc = config_cdc_init();
    printk("CDC init rc=%d\n", cdc_rc);

    /* The loop runs at ~8 ms so the button ladders scan + debounce at the
     * cadence buttons.c expects (3-read debounce ~24 ms, DFU hold ~1.2 s). The
     * slower behaviours below are re-throttled to that tick:
     *   •• hold ~3 s   -> 375 ticks    LED heartbeat ~250 ms -> every 31 ticks
     *   •• short tap   -> held 1..40 ticks (<= ~320 ms)
     *
     * •• FUNCTION BUTTON GESTURE (spec §8 count-dial — replaces the old single-tap
     * round-robin):
     *   - HOLD ~3 s (>= 375 ticks)  -> SYSTEM_OFF back to bootloader (power off).
     *   - SHORT TAP (held 1..40 ticks, i.e. <= ~320 ms, then released) is fed to
     *     the dial FSM (dial.c) as tap_edge=1. The dial then decides:
     *        1 tap        -> RELATIVE +1 (lib_bank_cycle, the old round-robin)
     *        2..8 taps    -> ABSOLUTE jump to within-bank profile N (count-1).
     *   This re-uses the single •• button: a quick dot-dot dial cycles/jumps
     *   profiles, a long press powers off. The tap upper bound is TIGHTENED from
     *   <62 to <=40 ticks (spec §8) to widen the margin from rapid dialing to the
     *   power-off hold; the dead band is now 41..374 ticks (does nothing on
     *   release), so a "medium" press the user aborts before the 3 s power
     *   threshold neither dials a profile nor powers off — avoids accidental
     *   switches while the user is on their way to a power-off hold. */
    uint32_t held = 0;
    uint32_t tick = 0;
    /* Honor the •• button only AFTER it has read released (high) at least once
     * since boot. On a low/marginal battery the pin can read low at power-on
     * (stuck, noisy, or brownout-glitched); without this guard that would count
     * straight toward the 3 s power-off and kill the boot. */
    int func_armed = 0;
    /* Track the active profile index across ticks so we can re-arm the faders
     * uniformly on ANY active-slot change — a •• short-tap, or a host
     * setactive/write/reset that lands on a different slot — without each call
     * site having to remember to do it. Seed from the boot-loaded active index
     * so the first loop iteration doesn't spuriously re-arm. */
    uint8_t last_active = librarian_active_index();
#if FELDD_SHIFT_TRIGGER_PLAY
    /* Double-tap-PLAY shift latch (test trigger). Pure state machine in
     * gesture.c; stepped once per tick from the button-scan block below. */
    gesture_t shift_gesture;
    gesture_init(&shift_gesture);
#endif
    /* •• count-dial (spec §8). Pure burst FSM in dial.c; stepped EVERY tick from
     * the •• handler below with tap_edge=1 only on a classified short-tap release.
     * It owns the relative-vs-absolute decision + the 8-clamp; main.c owns only
     * the tap CLASSIFIER and the commit-side librarian write. */
    dial_t dial;
    dial_init(&dial);
    /* Track-row dial/CONFIRM display state. While a burst is in flight the loop
     * renders dial.count live; after a commit it renders the committed index+1
     * for a short non-blocking CONFIRM flash. dial_disp_count = the count to draw
     * this tick (0 = nothing, fall back to layer rest); dial_confirm_ticks counts
     * a post-commit flash down to 0. CONFIRM ~120 ms (spec §8) ~= 15 ticks @ 8 ms. */
    unsigned char dial_disp_count   = 0;
    int           dial_confirm_ticks = 0;
    const int     DIAL_CONFIRM_TICKS = 15;   /* ~120 ms non-blocking CONFIRM flash */
    /* (0.9.1 LED redesign) SIDE-row MODE-FLASH state. The side row permanently
     * shows the LAYER (one LED at gesture_layer()); a successful mode switch arms
     * a brief, NON-BLOCKING flash of the NEW mode's pattern (captured in
     * mode_flash_mode), counted down by mode_flash_ticks exactly like the proven
     * front-row dial_confirm_ticks deadline. When it hits 0 the side row
     * re-asserts the layer LED on the next tick automatically. The replacement for
     * the deleted blocking mode_confirm_chase(). */
    int           mode_flash_ticks  = 0;
    unsigned char mode_flash_mode   = 0;
    /* "Calm middle" tunables (A/B on hardware without code surgery, per the
     * approved decision). MODE_FLASH_TICKS = flash window length (~150 ticks ~=
     * 1.2 s @ ~8 ms/tick). MODE_FLASH_STYLE = 0 PULSE (solid pattern the whole
     * window, the calm default) / 1 BLINK (toggle on (tick/12)&1). */
    const int     MODE_FLASH_TICKS  = 150;   /* ~1.2 s non-blocking mode-flash */
    const int     MODE_FLASH_STYLE  = 0;     /* 0 = PULSE (solid), 1 = BLINK */
    for (;;) {
        feed_wdt();

        /* Service the CDC config protocol: drain any host request lines and
         * write their responses. Non-blocking (returns at once if no RX). */
        config_cdc_poll();

        /* •• function button: long hold powers off; short taps feed the count-
         * dial (spec §8). tap_edge fires for exactly one tick on a classified
         * short-tap RELEASE; the dial FSM is stepped EVERY tick (below) whether
         * or not a tap landed, so its inter-tap / commit timers advance. */
        int func_low = (nrf_gpio_pin_read(SP1_FUNC_BTN) == 0);
        int tap_edge = 0;
        if (!func_armed) {
            /* Not armed yet: ignore •• entirely until it reads released once. */
            if (!func_low) {
                func_armed = 1;
            }
            held = 0;
        } else if (func_low) {
            if (++held >= 375) {
                enter_bootloader();   /* never returns (SYSTEM_OFF) */
            }
        } else {
            /* Release edge. A SHORT tap (held 1..40 ticks, <= ~320 ms) is one
             * dial tap: classify it and raise tap_edge for this single tick. The
             * dead band (41..374) does nothing — neither a dial tap nor a
             * power-off. held==0 means the button was never down this iteration
             * (not a release) so we skip. The TIGHTER <=40 bound (was <62, spec
             * §8) widens the margin to the 375-tick power-off so rapid dialing
             * can't bleed into a power-off hold. */
            if (held >= 1 && held <= 40) {
                tap_edge = 1;
            }
            held = 0;
        }

        /* Step the count-dial FSM once per tick (dial.c, host-tested). It counts
         * taps, decides relative-vs-absolute, and clamps the count to 8 — main.c
         * only commits the result to the librarian. */
        dial_result_t dres = dial_step(&dial, tap_edge);
        if (dres.kind == DIAL_RELATIVE_NEXT) {
            /* Lone tap = today's within-bank round-robin (+1). librarian_active_
             * index() is the WITHIN-bank index (0..7) and librarian_set_active()
             * takes a WITHIN-bank index, so the cycle wraps inside the bank of 8
             * (7->0) via lib_bank_cycle() — NOT % NUM_PROFILES (=16), which would
             * feed 8 into set_active and get -EINVAL, stranding the last profile
             * of every bank (host-tested test_lib_bank.c::t_cycle_wraps_within_bank). */
            uint8_t next = lib_bank_cycle(librarian_active_index());
            if (librarian_set_active(next) == 0) {
                /* Phase-0 item 4: push the new active profile to any connected
                 * host immediately, independent of the fader/button monitor flag,
                 * so the web tool never shows a stale active marker after an
                 * on-device •• dial. (No printk here: this fires mid-session and
                 * would inject a non-JSON line into the CDC protocol stream.) */
                config_cdc_monitor_active(librarian_active_index());
                /* Arm the non-blocking CONFIRM flash on the track row (the cue
                 * the old blocking profile_blink used to chase). */
                dial_confirm_ticks = DIAL_CONFIRM_TICKS;
            }
        } else if (dres.kind == DIAL_ABSOLUTE) {
            /* Burst of 2..8 taps = absolute jump to within-bank profile N. The
             * dial FSM already clamped count to 8, so count-1 is 0..7 — feed it
             * DIRECTLY to librarian_set_active(). Spec §8 gotchas: NEVER feed the
             * raw count (8 -> -EINVAL) and NEVER route through lib_bank_clamp()
             * (it resets >=8 to 0, silently jumping a stray 8 to profile 1). */
            if (librarian_set_active((uint8_t)(dres.count - 1)) == 0) {
                config_cdc_monitor_active(librarian_active_index());
                dial_confirm_ticks = DIAL_CONFIRM_TICKS;
            }
        }

        /* If the active profile changed this tick (via the •• tap above OR a
         * host setactive/write/reset serviced by config_cdc_poll()), re-arm
         * every fader so the new profile's mapping re-emits at the faders'
         * current physical positions on the next read — otherwise the unchanged
         * raw codes would stay suppressed and the new CCs would never reach the
         * host/OP-XY until each fader was physically moved. */
        uint8_t now_active = librarian_active_index();
        if (now_active != last_active) {
            faders_rearm();
            /* New profile = different CCs: drop soft-takeover memory so each bank
             * re-seeds and emits immediately (the prior jump-on-switch behavior),
             * not catch against another profile's stale values. */
            for (int i = 0; i < NUM_FADERS; i++) {
                g_takeover[i].pending = 0;
                g_takeover_arm[i] = 0;
                g_bank_valid[i][0] = 0;
                g_bank_valid[i][1] = 0;
            }
            last_active = now_active;
        }

        /* Shift bank selector for this tick: 0 unless the PLAY double-tap latch
         * is engaged (FELDD_SHIFT_TRIGGER_PLAY). Read once here — state as of the
         * previous tick's gesture step — so the faders and the non-PLAY buttons
         * below all use one consistent value; the latch itself is advanced at the
         * end of the button-scan block (and re-arms the faders on a flip). */
#if FELDD_SHIFT_TRIGGER_PLAY
        int shift_now = gesture_latched(&shift_gesture);
#else
        const int shift_now = 0;
#endif

        /* Scan both ladders FIRST (buttons before faders, like the verified
         * looper). Each edge -> the mapping engine -> TRS MIDI; one scan can emit
         * up to 4 edges (a release + a press on each ladder).
         * SHIFT LAYER (test trigger, FELDD_SHIFT_TRIGGER_PLAY): the shift bank is
         * driven by a double-tap on PLAY (button 0). PLAY is reserved for this —
         * its press edges feed the gesture detector and it never reaches
         * map_button, so it can't double-fire MIDI (it does still report to the
         * live monitor). When the latch flips, the faders arm soft-takeover so the
         * new bank holds its last value until the fader crosses it. With
         * FELDD_SHIFT_TRIGGER_PLAY=0, PLAY maps normally and shift_now is a
         * constant 0 (stock behaviour). */
        struct button_event evt[4];
        int ne = buttons_scan(evt, 4);
        /* PLAY-held = the mode-gesture modifier (phase 2b moved off •• onto PLAY, spec
         * §5). PLAY is tracks-ladder idx 0, so its COMMITTED/debounced hold reads as
         * buttons_track_committed()==0 (the tracks ladder reports one button at a time;
         * 0 = PLAY held, 1..4 = a Track button, -1 = none). Using the committed state
         * (not the raw press) means a momentary glitch can't spuriously flip the mode. */
        int play_held = (buttons_track_committed() == 0);
        /* Device mode for THIS tick's routing + the mode-LED render below. Read the
         * RAM hot copy once; a flip in this scan updates it locally so the same-tick
         * render + any later edge already see the new mode. */
        int mode_now = librarian_mode();
#if FELDD_SHIFT_TRIGGER_PLAY
        int play_press = 0;
        /* "A rocker arrived during this PLAY hold" — set on the tick mode_decide
         * consumes a FWD/RWD (md.consumed). Fed to gesture_step below so the
         * gesture FSM latches rocker_seen and VOIDS any pending profile peek (a
         * mode switch is not a peek). This folds in the old explicit
         * gesture_disarm(&shift_gesture) call (spec §3). */
        int rocker_consumed_this_hold = 0;
        for (int i = 0; i < ne; i++) {
            uint8_t idx = evt[i].idx;
            int pressed = evt[i].pressed;
            if (idx == 0) {                  /* PLAY: reserved as the shift trigger */
                if (pressed) {
                    play_press = 1;
                }
                config_cdc_monitor_button(0, pressed);  /* show PLAY pressed */
                continue;
            }
            /* PLAY-held + FWD/RWD = the global mode toggle (spec §5). mode_decide()
             * owns the pure, modifier-agnostic logic (host-tested in test_mode.c);
             * here PLAY-held is the modifier. On a consumed edge main.c persists the
             * new mode, arms the NON-BLOCKING side-row MODE-FLASH (0.9.1), pushes a
             * mon{k:"mode",v} frame to any host, and flags
             * rocker_consumed_this_hold so the gesture FSM (stepped after this loop)
             * VOIDS any pending profile peek and clears a half-formed shift double-tap
             * — the inline gesture_disarm() is now folded into gesture_step under that
             * signal (spec §3). The •• power/profile gestures are untouched by a flip. */
            struct mode_decision md = mode_decide(play_held, idx, pressed);
            if (md.consumed) {
                rocker_consumed_this_hold = 1;
                if (md.set && librarian_set_mode(md.which) == 0) {
                    mode_now = md.which;
                    /* (0.9.1) Arm the NON-BLOCKING side-row MODE-FLASH for the NEW
                     * mode instead of the deleted blocking chase. The per-tick
                     * side render shows the captured pattern for MODE_FLASH_TICKS,
                     * then yields back to the permanent layer LED. */
                    mode_flash_mode  = md.which;
                    mode_flash_ticks = MODE_FLASH_TICKS;
                    config_cdc_monitor_mode(md.which);
                    /* A mode flip switches profile BANK, so the active *profile*
                     * changed even when the within index did not (e.g. both banks
                     * resting at within 0). The per-tick now_active!=last_active
                     * re-arm above keys off the WITHIN index and would miss that,
                     * so re-arm the faders HERE for the new bank's CCs, clear the
                     * soft-takeover memory (different profile = different values),
                     * push the bank's active so the web re-scopes, and reset
                     * last_active so the next tick doesn't double-rearm. */
                    config_cdc_monitor_active(librarian_active_index());  /* bank's active */
                    faders_rearm();                                       /* new bank = new CCs */
                    for (int fi = 0; fi < NUM_FADERS; fi++) {
                        g_takeover[fi].pending = 0;
                        g_takeover_arm[fi] = 0;
                        g_bank_valid[fi][0] = 0;
                        g_bank_valid[fi][1] = 0;
                    }
                    last_active = librarian_active_index();   /* don't double-rearm next tick */
                }
                config_cdc_monitor_button(idx, pressed);  /* still report the edge */
                continue;
            }
            /* Normal routing: KEYBOARD types the per-button HID key on press (mod +
             * usage from the keymap; usb_hid_send_key drops silently if no HID
             * host is bound, and an unbound key (0x00) is a harmless empty report).
             * SHIFT LAYER (v4): the same double-tap-PLAY latch the MIDI path uses
             * (shift_now) selects the SECOND keymap — button_key_shift/
             * button_mod_shift — so Keyboard mode gets a Layer 2 exactly like MIDI.
             * MIDI maps the button as before. Faders are ALWAYS MIDI (handled
             * below), regardless of mode. */
            if (mode_now == MODE_KEYBOARD) {
                if (pressed) {
                    const struct profile *p = librarian_active();
                    usb_hid_send_key(
                        shift_now ? p->button_mod_shift[idx] : p->button_mod[idx],
                        shift_now ? p->button_key_shift[idx] : p->button_key[idx]);
                }
            } else {
                map_button(librarian_active(), idx, pressed, shift_now,
                           midi_out_send, NULL);
            }
            config_cdc_monitor_button(idx, pressed);  /* mon frame */
        }
        /* Advance the PLAY gesture FSM once per tick with the full signature (spec
         * §3): the press edge, the DEBOUNCED hold (buttons_track_committed()==0,
         * already in play_held), and whether a rocker was consumed this hold (which
         * voids a would-be peek). It returns one of three outcomes:
         *   GESTURE_LAYER_STEP   - a double-tap stepped the layer (was LATCH_TOGGLED)
         *   GESTURE_PEEK_PROFILE - a hold-PLAY-alone released past the peek plateau */
        int gres = gesture_step(&shift_gesture, play_press, play_held,
                                rocker_consumed_this_hold);
        if (gres == GESTURE_LAYER_STEP) {
            /* Layer flipped: ARM soft-takeover on every fader (deferred to the
             * next fader read, which has a fresh position) instead of re-emitting.
             * Each fader then holds the new bank's last value until the physical
             * fader CROSSES it - no jump. (faders_rearm() here would jump the CC
             * to the fader's current spot, the old behavior.) */
            for (int i = 0; i < NUM_FADERS; i++) {
                g_takeover_arm[i] = 1;
            }
        } else if (gres == GESTURE_PEEK_PROFILE) {
            /* Hold-PLAY-alone peek: flash the CURRENT within-bank profile on the
             * track row, non-blocking, via the same CONFIRM render the dial uses
             * (spec §2/§4/§8 — peek and dial share the track row, drawn by
             * profile_track_render below). We do NOT depend on any unbuilt peek
             * FSM here; we just re-show the current profile = active index + 1.
             * dial_disp_count is set below from this; arm the same flash timer. */
            dial_confirm_ticks = DIAL_CONFIRM_TICKS;
        }
#else
        for (int i = 0; i < ne; i++) {
            uint8_t idx = evt[i].idx;
            int pressed = evt[i].pressed;
            /* PLAY-held + FWD/RWD = the global mode toggle — see the
             * FELDD_SHIFT_TRIGGER_PLAY arm above for the full rationale. Consume the
             * edge, persist the mode, arm the NON-BLOCKING side-row MODE-FLASH
             * (0.9.1), push to the host. (No shift detector in this build, so no
             * gesture_disarm needed — PLAY maps normally here, not the shift
             * trigger.) */
            struct mode_decision md = mode_decide(play_held, idx, pressed);
            if (md.consumed) {
                if (md.set && librarian_set_mode(md.which) == 0) {
                    mode_now = md.which;
                    /* (0.9.1) Arm the NON-BLOCKING side-row MODE-FLASH (same as the
                     * FELDD_SHIFT_TRIGGER_PLAY arm above) — the deleted blocking
                     * chase is replaced by the per-tick side render. */
                    mode_flash_mode  = md.which;
                    mode_flash_ticks = MODE_FLASH_TICKS;
                    config_cdc_monitor_mode(md.which);
                    /* Mode flip switches profile BANK -> active profile changed
                     * even if the within index didn't; re-arm faders for the new
                     * bank's CCs, clear soft-takeover memory, push the bank's
                     * active, and reset last_active so the next tick can't
                     * double-rearm. (No shift detector in this build -> no
                     * gesture_disarm, unlike the FELDD_SHIFT_TRIGGER_PLAY arm.) */
                    config_cdc_monitor_active(librarian_active_index());  /* bank's active */
                    faders_rearm();                                       /* new bank = new CCs */
                    for (int fi = 0; fi < NUM_FADERS; fi++) {
                        g_takeover[fi].pending = 0;
                        g_takeover_arm[fi] = 0;
                        g_bank_valid[fi][0] = 0;
                        g_bank_valid[fi][1] = 0;
                    }
                    last_active = librarian_active_index();   /* don't double-rearm next tick */
                }
                config_cdc_monitor_button(idx, pressed);
                continue;
            }
            /* Normal routing: KEYBOARD types the per-button HID key on press; MIDI
             * maps (here PLAY maps normally — no shift trigger in this build). */
            if (mode_now == MODE_KEYBOARD) {
                if (pressed) {
                    const struct profile *p = librarian_active();
                    usb_hid_send_key(p->button_mod[idx], p->button_key[idx]);
                }
            } else {
                map_button(librarian_active(), idx, pressed, shift_now,
                           midi_out_send, NULL);
            }
            config_cdc_monitor_button(idx, pressed);  /* mon frame */
        }
#endif

        /* Track1+4 held the full ~1.2 s -> reset into the bootloader for DFU. */
        if (buttons_dfu_held()) {
            enter_dfu();
        }

        /* TRACK-row (FRONT) render — strict per-tick priority, re-derived every
         * tick (0.9.1 LED redesign, spec §4). The front row is now the DIAL/PEEK +
         * plain PRESS-FEEDBACK surface; the LAYER moved to the SIDE row below and
         * the press-INVERSION is removed. Highest priority wins:
         *   1. DFU all-4-solid  — handled by enter_dfu() above (never returns), so
         *      it is not a per-tick case here; the boot sweep also transiently owns
         *      these pins before the loop starts.
         *   2. DIAL / PEEK flash — while a •• burst is in flight OR a post-commit /
         *      peek CONFIRM flash is counting down, dial_track_pattern() owns the
         *      row (counts 1-4 solid, 5-8 solid+blink). UNCHANGED.
         *   3. PRESS feedback (PLAIN) — when a Track button is committed, light
         *      ONLY that button's own front LED (on = i == pressed_idx); no
         *      inversion, no layer, no home concept. The ladder reads one button at
         *      a time -> at most one Track held, no two-button case (spec §4).
         *   4. ALL-DARK at rest — the layer lives on the side row now, so the front
         *      row is the intended calm idle (all off) when nothing is pressed. */
        const uint32_t track_leds[4] = {
            SP1_TRACK_LED1, SP1_TRACK_LED2, SP1_TRACK_LED3, SP1_TRACK_LED4
        };

        /* Decide what (if anything) the dial/peek flash should draw this tick.
         * A live burst draws its running count; otherwise a CONFIRM/peek timer
         * draws the committed/current within-bank profile (index+1). 0 = nothing,
         * fall through to press/layer. */
        if (dial.active && dial.count > 0) {
            dial_disp_count = dial.count;           /* live burst -> running count */
        } else if (dial_confirm_ticks > 0) {
            dial_disp_count = (unsigned char)(librarian_active_index() + 1);
            dial_confirm_ticks--;                   /* non-blocking countdown */
        } else {
            dial_disp_count = 0;                     /* idle -> press/layer below */
        }

        if (dial_disp_count > 0) {
            /* Priority 2: dial / peek flash owns the whole track row. Use the live
             * scan tick for the 5-8 blink phase (dial_track_pattern). */
            profile_track_render(dial_disp_count, (unsigned char)tick);
        } else {
            /* Priority 3 (PLAIN press feedback) + priority 4 (all-dark rest).
             * (0.9.1 LED redesign, spec §4) The LAYER + the press-INVERSION are
             * GONE from the front row: when a Track button is committed, light ONLY
             * its own front LED (on = i == pressed_idx); when nothing is held the
             * row is all-dark (the layer LED lives on the side row now). The ladder
             * reads one button at a time, so there is never a two-button case. This
             * branch is identical in BOTH build arms (no gesture/layer read), so
             * FELDD_SHIFT_TRIGGER_PLAY no longer guards anything here. */
            int trk_held = buttons_track_committed();        /* 1..4 = a Track, else none */
            int pressed_idx = (trk_held >= 1 && trk_held <= 4) ? (trk_held - 1) : -1;
            for (int i = 0; i < 4; i++) {
                int on = (i == pressed_idx);                 /* plain: only the pressed LED */
                if (on) {
                    nrf_gpio_pin_set(track_leds[i]);
                } else {
                    nrf_gpio_pin_clear(track_leds[i]);
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
         *   2. LAYER rest (permanent) — one side LED lit at the GLOBAL layer
         *      (gesture_layer(), natural ordering: layer 0 -> SP1_PLAY_LED1). The
         *      layer is unchanged across a mode switch, so the flash yields back to
         *      the same layer LED. Never all-dark in normal operation.
         * The pure side_led_pattern() helper (side_led.c, host-tested) owns the
         * render math; main.c is the GPIO arbiter + the deadline countdown. Under
         * FELDD_SHIFT_TRIGGER_PLAY=0 there is no PLAY gesture FSM, so the layer pins
         * to 0 (SP1_PLAY_LED1 = the single resting layer), mirroring the old
         * front-row guard. */
#if FELDD_SHIFT_TRIGGER_PLAY
        unsigned char side_layer = (unsigned char)gesture_layer(&shift_gesture);
#else
        const unsigned char side_layer = 0;   /* single layer -> SP1_PLAY_LED1 rest */
#endif
        {
            unsigned char side_out[4];
            side_led_pattern(side_layer, (unsigned int)mode_flash_ticks,
                             mode_flash_mode, (MODE_FLASH_STYLE == 1),
                             (unsigned char)tick, side_out);
#ifdef FELDD_MODE_LED_SINGLE
            /* Fallback (build with -DFELDD_MODE_LED_SINGLE): collapse the whole side
             * indicator onto the fully-validated P1.13 (SP1_LED1 = SP1_PLAY_LED4)
             * and NEVER drive the 3 spare side pins. The OR over side_out reproduces
             * the helper's intent on one pin: LAYER rest -> P1.13 solid (the layer
             * dot), MODE-FLASH -> P1.13 pulses/blinks with the flash window. Used if
             * the spare side pins misbehave on the first bench flash (spec §2.1). */
            if (side_out[0] || side_out[1] || side_out[2] || side_out[3]) {
                nrf_gpio_pin_set(SP1_LED1);
            } else {
                nrf_gpio_pin_clear(SP1_LED1);
            }
#else
            const uint32_t play_leds[4] = {
                SP1_PLAY_LED1, SP1_PLAY_LED2, SP1_PLAY_LED3, SP1_PLAY_LED4
            };
            for (int i = 0; i < 4; i++) {
                if (side_out[i]) {
                    nrf_gpio_pin_set(play_leds[i]);
                } else {
                    nrf_gpio_pin_clear(play_leds[i]);
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

        /* Faders -> CC, AFTER the buttons. SKIP the read while a button is pulling
         * the shared BTN_COM rail (+ 2 ticks after it lets go): a held button sags
         * that rail, and because the SAADC ref is internal (not ratiometric) every
         * fader ADC read drops ~1-2 LSB. We gate on buttons_rail_loaded() — the
         * INSTANTANEOUS raw ladder read captured by buttons_scan() above — NOT the
         * 3-read committed state: the committed state lags the press by ~24 ms, so
         * the drooped reads of those first ticks leaked out as spurious CC dips
         * (the v0.5.2 regression; petercolombo's bench report, 2026-06-18).
         * Skipping the UPDATE — not just the emit — keeps each fader's
         * last_sent_raw at its true pre-press value, so the recovered read
         * duplicate-suppresses instead of emitting a spurious correction. */
        static uint8_t fader_settle;
        if (buttons_rail_loaded()) {
            fader_settle = 2;
        } else if (fader_settle > 0) {
            fader_settle--;
        }
        if (fader_settle == 0) {
            for (int idx = 0; idx < NUM_FADERS; idx++) {
                int raw = controls_read_raw(2 + idx);
                if (raw < 0) {
                    continue;   /* read error this tick; try again next loop */
                }
                uint8_t phys_cc = fader_raw_to_cc((uint16_t)raw);

                /* Soft-takeover: a pending arm from a shift toggle sets up the
                 * catch against the new bank's last value (or seeds if this bank
                 * was never sent in this profile). initialized=0 forces a clean
                 * re-emit the moment the catch releases. */
                if (g_takeover_arm[idx]) {
                    g_takeover_arm[idx] = 0;
                    if (g_bank_valid[idx][shift_now]) {
                        takeover_arm(&g_takeover[idx], phys_cc,
                                     g_bank_last_cc[idx][shift_now]);
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

                int cc = fader_update(&g_fader[idx], (uint16_t)raw);
                if (cc >= 0) {
                    map_fader(librarian_active(), idx, cc, shift_now, midi_out_send, NULL);
                    config_cdc_monitor_fader(idx, cc);   /* mon frame if monitoring */
                    g_bank_last_cc[idx][shift_now] = (uint8_t)cc;
                    g_bank_valid[idx][shift_now] = 1;
                }
            }
        }

        /* (0.9.1 LED redesign: the side row shows the LAYER permanently (one LED at
         * gesture_layer()) + a temporary MODE-FLASH on a switch — rendered above by
         * side_led_pattern(). The default build drives the 4 side play LEDs; the
         * -DFELDD_MODE_LED_SINGLE fallback collapses the indicator onto SP1_LED1/
         * P1.13 only. The front/track row is press-feedback + dial/peek, all-dark
         * at rest.) */
        tick++;
        k_msleep(8);
    }
    return 0;
}
