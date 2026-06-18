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
 * M4.1 adds: USB-MIDI 2.0 (usbd_midi2) to the same composite as the CDC console
 * (both ride one usbd context, brought up by usbdev_start), and a USB sink in
 * midi_out so the mapping engine emits to BOTH the TRS UART and USB-MIDI.
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
#include "librarian.h"
#include "config_cdc.h"
#include "gesture.h"

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

static void feed_wdt(void) { for (int c = 0; c < 8; c++) NRF_WDT->RR[c] = WDT_RR_RR_Reload; }

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

/* M5.2 profile-switch feedback: blink the 4 track LEDs (idx+1) times to
 * indicate the newly-selected profile index (1 blink = profile 0, ... 8 blinks
 * = profile 7). This runs INSIDE the control loop's tap-release handler, so it
 * briefly blocks the 8 ms cadence; we feed the watchdog between blinks so a
 * full 8-blink cue (~1.3 s worst case) can never stall the WDT (5 s budget). */
static void profile_blink(uint8_t index)
{
    nrf_gpio_cfg_output(SP1_TRACK_LED1);
    nrf_gpio_cfg_output(SP1_TRACK_LED2);
    nrf_gpio_cfg_output(SP1_TRACK_LED3);
    nrf_gpio_cfg_output(SP1_TRACK_LED4);
    for (int b = 0; b <= (int)index; b++) {
        feed_wdt();
        nrf_gpio_pin_set(SP1_TRACK_LED1);
        nrf_gpio_pin_set(SP1_TRACK_LED2);
        nrf_gpio_pin_set(SP1_TRACK_LED3);
        nrf_gpio_pin_set(SP1_TRACK_LED4);
        k_msleep(80);
        feed_wdt();
        nrf_gpio_pin_clear(SP1_TRACK_LED1);
        nrf_gpio_pin_clear(SP1_TRACK_LED2);
        nrf_gpio_pin_clear(SP1_TRACK_LED3);
        nrf_gpio_pin_clear(SP1_TRACK_LED4);
        k_msleep(80);
    }
}

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
#define FELDD_BOOT_SIG_SWEEPS 2   /* feldd v0.4.0: 2 sweeps */
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
     *   •• short tap   -> < ~500 ms (< 62 ticks)            ADC diag ~500 ms -> every 62 ticks
     *
     * •• FUNCTION BUTTON GESTURE (M5.2):
     *   - HOLD ~3 s (>= 375 ticks)  -> SYSTEM_OFF back to bootloader (power off).
     *   - SHORT TAP (held 1..61 ticks, i.e. < ~500 ms, then released)
     *        -> advance to the next profile: librarian_set_active((i+1) % N),
     *           then blink the track LEDs (new index + 1) times as the cue.
     *   This re-uses the single •• button: a quick press cycles profiles, a long
     *   press powers off. The dead band (62..374 ticks) does nothing on release,
     *   so a "medium" press that the user aborts before the 3 s power threshold
     *   neither switches a profile nor powers off — avoids accidental switches
     *   while the user is on their way to a power-off hold. */
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
    for (;;) {
        feed_wdt();

        /* F2: drain the USB MIDI last-value-wins pending table first, so a
         * backed-up USB TX ring recovers and any CC/value that hit -ENOBUFS
         * last tick lands before we read new fader values this tick. */
        midi_out_pump();

        /* Service the CDC config protocol: drain any host request lines and
         * write their responses. Non-blocking (returns at once if no RX). */
        config_cdc_poll();

        /* •• function button: long hold powers off; short tap (on release)
         * advances the active profile. */
        int func_low = (nrf_gpio_pin_read(SP1_FUNC_BTN) == 0);
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
            /* Release edge: a short tap (< ~500 ms) advances to the next
             * profile. held==0 means the button was never down this iteration,
             * so it is not a release and we skip. */
            if (held >= 1 && held < 62) {
                uint8_t next = (uint8_t)((librarian_active_index() + 1) % NUM_PROFILES);
                /* On success blink the new index on the track LEDs (the user
                 * cue). No printk here: this fires mid-session on a •• tap and
                 * would inject a non-JSON line into the CDC protocol stream. */
                if (librarian_set_active(next) == 0) {
                    /* Phase-0 item 4: push the new active profile to any
                     * connected host immediately, independent of the fader/
                     * button monitor flag, so the web tool never shows a stale
                     * active marker after an on-device •• switch. */
                    config_cdc_monitor_active(librarian_active_index());
                    profile_blink(librarian_active_index());
                }
            }
            held = 0;
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

        /* Faders -> CC. Read each fader's 12-bit code (controls idx 2..5 are
         * faders 1..4), run the send-on-change/jitter filter, and on a real
         * change push it through the mapping engine into the TRS MIDI sink. */
        for (int idx = 0; idx < NUM_FADERS; idx++) {
            int raw = controls_read_raw(2 + idx);
            if (raw < 0) {
                continue;   /* read error this tick; try again next loop */
            }
            int cc = fader_update(&g_fader[idx], (uint16_t)raw);
            if (cc >= 0) {
                map_fader(librarian_active(), idx, cc, shift_now, midi_out_send, NULL);
                config_cdc_monitor_fader(idx, cc);   /* mon frame if monitoring */
            }
        }

        /* Scan both ladders; each edge -> the mapping engine -> TRS MIDI. One
         * scan can emit up to 4 edges (a release + a press on each of the two
         * ladders).
         * SHIFT LAYER (test trigger, FELDD_SHIFT_TRIGGER_PLAY): rather than the
         * overloaded •• button (short tap = next profile, long hold = power off),
         * the shift bank is driven by a double-tap on PLAY (button 0). PLAY is
         * reserved for this — its press edges feed the gesture detector and it
         * never reaches map_button, so it can't double-fire MIDI. The other
         * buttons + the faders use `shift_now` (read above). When the latch
         * flips, re-arm the faders so the new bank's CCs emit at current
         * positions. With FELDD_SHIFT_TRIGGER_PLAY=0, PLAY maps normally and
         * shift_now is a constant 0 (stock behaviour). */
        struct button_event evt[4];
        int ne = buttons_scan(evt, 4);
#if FELDD_SHIFT_TRIGGER_PLAY
        int play_press = 0;
        for (int i = 0; i < ne; i++) {
            if (evt[i].idx == 0) {           /* PLAY: reserved as the shift trigger */
                if (evt[i].pressed) {
                    play_press = 1;
                }
                continue;                    /* never emit PLAY's normal mapping */
            }
            map_button(librarian_active(), evt[i].idx, evt[i].pressed, shift_now,
                       midi_out_send, NULL);
            config_cdc_monitor_button(evt[i].idx, evt[i].pressed);  /* mon frame */
        }
        if (gesture_step(&shift_gesture, play_press) == GESTURE_LATCH_TOGGLED) {
            faders_rearm();   /* re-emit the new bank at the faders' positions */
        }
#else
        for (int i = 0; i < ne; i++) {
            map_button(librarian_active(), evt[i].idx, evt[i].pressed, shift_now,
                       midi_out_send, NULL);
            config_cdc_monitor_button(evt[i].idx, evt[i].pressed);  /* mon frame */
        }
#endif

        /* Track1+4 held the full ~1.2 s -> reset into the bootloader for DFU. */
        if (buttons_dfu_held()) {
            enter_dfu();
        }

        if ((tick % 31) == 0) {
            nrf_gpio_pin_toggle(SP1_LED1);
        }
        tick++;
        k_msleep(8);
    }
    return 0;
}
