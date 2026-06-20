/*
 * buttons.c — decode the SP-1's two resistor-ladder ADC inputs into the 9
 * logical buttons, with a 3-read sticky debounce per ladder, plus detection of
 * the Track1+4 "enter DFU" combo band.
 *
 * Logical button indices (MUST match the mapping engine's button[9] order and
 * the protocol's button indices):
 *     0=Play  1=Track1  2=Track2  3=Track3  4=Track4
 *     5=VolUp 6=VolDown 7=FWD     8=RWD
 * The •• function button is a DIRECT GPIO (P0.27) handled in main.c, NOT one of
 * these 9 — it is power/bootloader only.
 *
 * Thresholds come from the chattock looper's VERIFIED decode (decode_tracks /
 * decode_vol in sp1-tape-looper/firmware/src/main.c). HARDWARE CALIBRATION of
 * the exact ADC plateaus is DEFERRED until Unit A is on the bench; these are the
 * looper's confirmed defaults.
 *
 * The decode + debounce is PURE INTEGER LOGIC (see *_pure functions below) and
 * depends only on the raw 0..4095 ADC values, so it is host-testable. The only
 * Zephyr coupling is the I/O shell (buttons_init / controls_read_raw), compiled
 * out when BUTTONS_HOST_TEST is defined.
 */
#include "buttons.h"

/* --------------------------------------------------------------------------
 * Decode: raw ladder code -> a single active logical index, or -1 for none.
 * Only one button per ladder can be active at a time — that is how a resistor
 * ladder physically works (a second press just pulls a different parallel
 * resistance, landing on its own plateau, never two at once).
 * ------------------------------------------------------------------------ */

/* TRACKS ladder (controls_read_raw(0), AIN0). Looper decode_tracks():
 *   <110 NONE | <300 T1(~213) | <560 T2(~403) | <950 T3(~733)
 *   | <1500 T4(~1220) | else PLAY(~1823)
 * Mapped to our indices: Play=0, Track1..4 = 1..4. */
int buttons_decode_tracks_pure(int v)
{
    if (v <  110) return -1;   /* idle */
    if (v <  300) return 1;    /* Track1 */
    if (v <  560) return 2;    /* Track2 */
    if (v <  950) return 3;    /* Track3 */
    if (v < 1500) return 4;    /* Track4 */
    return 0;                  /* Play */
}

/* VOL ladder (controls_read_raw(1), AIN1). Looper decode_vol() yields four
 * distinct plateaus in ASCENDING raw order:
 *     <200 NONE | <560 (~404) | <950 (~729) | <1500 (~1220) | else (~1820)
 * The looper's own names (TEMPO_DOWN/VOL_DOWN/TEMPO_UP/VOL_UP) are looper-
 * specific. What is physically fixed is the PLATEAU ORDER; the logical name on
 * each plateau is a wiring assumption confirmable on hardware later.
 *
 * BENCH-CONFIRMED ASSIGNMENT (ascending raw -> our logical name). petercolombo
 * reported on a real unit (2026-06-18) that FWD and VolDown were swapped — the
 * outer two (RWD lowest, VolUp highest) were correct, the inner two reversed. So
 * the transport/volume pairs are interleaved on the ladder, not grouped as first
 * assumed:
 *     plateau ~404  -> RWD     (idx 8)
 *     plateau ~729  -> VolDown (idx 6)
 *     plateau ~1220 -> FWD     (idx 7)
 *     plateau ~1820 -> VolUp   (idx 5)
 * If a future bench disagrees, only this mapping table changes. */
int buttons_decode_vol_pure(int v)
{
    if (v <  200) return -1;   /* idle */
    if (v <  560) return 8;    /* RWD */
    if (v <  950) return 6;    /* VolDown */
    if (v < 1500) return 7;    /* FWD */
    return 5;                  /* VolUp */
}

/* Track1+4 DFU combo: pressing Track1 and Track4 together pulls a DISTINCT band
 * on the TRACKS ladder between T4 (~1220) and PLAY (~1823). Looper uses
 * 1280..1390. HARDWARE-CALIBRATE on the bench. */
#define DFU_BAND_LO 1280
#define DFU_BAND_HI 1390

/* ~1.2 s of consecutive in-band scans at the ~8 ms scan cadence. */
#define DFU_HOLD_SCANS 150

int buttons_in_dfu_band_pure(int v)
{
    return (v >= DFU_BAND_LO && v <= DFU_BAND_HI) ? 1 : 0;
}

/* --------------------------------------------------------------------------
 * Sticky 3-read debounce.
 *
 * A candidate decode must match on 3 CONSECUTIVE reads (~24 ms) before it
 * becomes the committed (settled) button; a lone glitch back to the held value
 * resets the counter, so a steady hold can never false-trigger and a single
 * noisy ADC sample can never look like a release. Mirrors the looper's verified
 * debounce. Returns the committed index after this read (-1 = none).
 * ------------------------------------------------------------------------ */
struct btn_debounce {
    int committed;   /* settled button index, -1 = none */
    int cand;        /* candidate awaiting confirmation */
    int cand_cnt;    /* consecutive reads matching cand */
};

void buttons_debounce_init(struct btn_debounce *d)
{
    d->committed = -1;
    d->cand = -1;
    d->cand_cnt = 0;
}

int buttons_debounce_step(struct btn_debounce *d, int raw_decode)
{
    if (raw_decode == d->committed) {
        d->cand_cnt = 0;            /* steady hold: drop any in-flight candidate */
    } else if (raw_decode == d->cand) {
        if (++d->cand_cnt >= 3) {   /* confirmed on 3 consecutive reads */
            d->committed = raw_decode;
            d->cand_cnt = 0;
        }
    } else {
        d->cand = raw_decode;       /* new candidate; start its count */
        d->cand_cnt = 1;
    }
    return d->committed;
}

/* --------------------------------------------------------------------------
 * Pure scan core: given the two raw ladder reads and persistent state, advance
 * both debouncers + the DFU hold counter and emit press/release edges. This is
 * the whole behaviour, with no Zephyr dependency, so the host test drives it
 * directly.
 * ------------------------------------------------------------------------ */
struct buttons_state {
    struct btn_debounce trk;   /* tracks ladder -> {Play,Track1..4} */
    struct btn_debounce vol;   /* vol ladder    -> {VolUp,VolDn,FWD,RWD} */
    int dfu_scans;             /* consecutive in-band scans of the combo */
    int dfu_held;              /* latched once DFU_HOLD_SCANS reached */
};

void buttons_state_init(struct buttons_state *s)
{
    buttons_debounce_init(&s->trk);
    buttons_debounce_init(&s->vol);
    s->dfu_scans = 0;
    s->dfu_held = 0;
}

/* Emit a single edge if capacity allows; returns the (possibly unchanged) count. */
static int emit_edge(struct button_event *evt, int cap, int n, int idx, int pressed)
{
    if (idx < 0 || n >= cap) {
        return n;
    }
    evt[n].idx = (uint8_t)idx;
    evt[n].pressed = (uint8_t)pressed;
    return n + 1;
}

/* Advance one ladder's debounce and emit a release for the old committed button
 * and a press for the new one when the committed value changes. */
static int scan_ladder(struct btn_debounce *d, int raw_decode,
                       struct button_event *evt, int cap, int n)
{
    int before = d->committed;
    int after = buttons_debounce_step(d, raw_decode);
    if (after != before) {
        n = emit_edge(evt, cap, n, before, 0);   /* release the old (if any) */
        n = emit_edge(evt, cap, n, after, 1);    /* press the new (if any) */
    }
    return n;
}

/* The full pure scan. trk_raw / vol_raw are the two ladder codes (0..4095).
 * Detects the DFU combo on the TRACKS ladder BEFORE the normal track decode, so
 * the combo band is never mistaken for a Track-4 press. Returns the number of
 * edges written to evt[]. */
int buttons_scan_pure(struct buttons_state *s, int trk_raw, int vol_raw,
                      struct button_event *evt, int cap)
{
    int n = 0;

    /* DFU combo first: while the band holds, the tracks ladder reports NONE so
     * neither Track4 nor Play can be (mis)pressed during the combo. */
    int trk_decode;
    if (buttons_in_dfu_band_pure(trk_raw)) {
        if (s->dfu_scans < DFU_HOLD_SCANS) {
            s->dfu_scans++;
        }
        if (s->dfu_scans >= DFU_HOLD_SCANS) {
            s->dfu_held = 1;
        }
        trk_decode = -1;
    } else {
        s->dfu_scans = 0;
        trk_decode = buttons_decode_tracks_pure(trk_raw);
    }

    n = scan_ladder(&s->trk, trk_decode, evt, cap, n);
    n = scan_ladder(&s->vol, buttons_decode_vol_pure(vol_raw), evt, cap, n);
    return n;
}

/* --------------------------------------------------------------------------
 * Zephyr I/O shell. Compiled out for the host test (which drives the pure core
 * above directly). The shell is intentionally thin: it only wires
 * controls_read_raw() into buttons_scan_pure() over module-static state.
 * ------------------------------------------------------------------------ */
#ifndef BUTTONS_HOST_TEST
#include "controls.h"

static struct buttons_state g_state;
static int g_rail_loaded;   /* instantaneous: any button pulling the rail this scan */

int buttons_init(void)
{
    buttons_state_init(&g_state);
    g_rail_loaded = 0;
    return 0;
}

int buttons_scan(struct button_event *evt, int cap)
{
    int trk_raw = controls_read_raw(0);   /* tracks ladder (AIN0) */
    int vol_raw = controls_read_raw(1);   /* vol ladder   (AIN1) */
    if (trk_raw < 0) trk_raw = 0;         /* a read error reads as idle */
    if (vol_raw < 0) vol_raw = 0;
    /* Capture the INSTANTANEOUS rail load (pre-debounce): a button pressed THIS
     * scan already pulls current through its ladder, sagging the shared BTN_COM
     * rail. The fader gate reads this rather than the 3-read committed state, so
     * it skips the drooped fader read on the very first sagged tick instead of
     * ~24 ms later. Thresholds mirror the decode idle bands (<110 tracks, <200
     * vol = nothing held); the Track1+4 DFU band (>110) also loads the rail. */
    g_rail_loaded = (trk_raw >= 110) || (vol_raw >= 200);
    return buttons_scan_pure(&g_state, trk_raw, vol_raw, evt, cap);
}

int buttons_dfu_held(void)
{
    return g_state.dfu_held;
}

/* True if THIS scan's raw ladder reads showed any button pulling the rail
 * (instantaneous, pre-debounce). The control loop gates the fader read on this:
 * a held button sags the shared BTN_COM rail, which (the SAADC ref is internal,
 * not ratiometric) drops all four fader reads ~1-2 LSB. The raw read catches the
 * press edge on the first sagged tick; the committed state would lag it ~24 ms
 * and leak the droop. */
int buttons_rail_loaded(void)
{
    return g_rail_loaded;
}

/* The tracks-ladder committed (debounced) button index: -1 idle, 0=Play,
 * 1..4 = Track1..4. Drives the track-LED button-press feedback in main.c. A
 * resistor ladder reports one button at a time, so at most one track is held. */
int buttons_track_committed(void)
{
    return g_state.trk.committed;
}
#endif /* BUTTONS_HOST_TEST */
