/*
 * controls.c — SAADC read of the SP-1's 4 faders + 2 button ladders + battery.
 *
 * The PLAY/track and Vol/FWD/RWD buttons are resistor ladders read on the
 * SAADC; the faders are linear pots; the battery rides an on-board divider.
 * All of these are only powered when BTN_COM (P1.10) is driven high, so we
 * raise that rail at init before any sampling. Channel order matches the
 * zephyr,user io-channels list in app.overlay (the chattock looper's VERIFIED
 * map): 0=ladder tracks(AIN0), 1=ladder vol(AIN1), 2..5=faders 1..4
 * (AIN3,6,2,7), 6=battery(AIN4).
 *
 * Adapted from the looper's ladder_read(): 2x oversample to quiet the rail.
 */
#include "controls.h"
#include <zephyr/drivers/adc.h>
#include <zephyr/devicetree.h>
#include <hal/nrf_gpio.h>
#include "sp1_board.h"

#define N_CH 7

static const struct adc_dt_spec ch[N_CH] = {
#define G(i) ADC_DT_SPEC_GET_BY_IDX(DT_PATH(zephyr_user), i)
    G(0), G(1), G(2), G(3), G(4), G(5), G(6)
#undef G
};

static int16_t sample;

int controls_init(void)
{
    nrf_gpio_cfg_output(SP1_BTN_COM);
    nrf_gpio_pin_set(SP1_BTN_COM);                  /* power the ladders/faders */
    for (int i = 0; i < N_CH; i++) {
        if (device_is_ready(ch[i].dev)) {
            adc_channel_setup_dt(&ch[i]);
        }
    }
    /* Deliberately NO SAADC offset calibration here. An earlier controls_calibrate()
     * ran nrfx_saadc_offset_calibrate against the still-charging BTN_COM rail,
     * baking a global offset that pushed PLAY's tracks-ladder plateau below its
     * decode band (PLAY never registered) and dragged the fader reads down. The
     * verified tape-looper never calibrates; we match it. (root-caused 2026-06-18
     * from petercolombo's bench report.) */
    return 0;
}

int controls_read_raw(int i)
{
    if (i < 0 || i >= N_CH) {
        return -1;
    }
    struct adc_sequence seq = {
        .buffer      = &sample,
        .buffer_size = sizeof(sample),
    };
    if (adc_sequence_init_dt(&ch[i], &seq) < 0) {
        return -1;
    }
    int32_t acc = 0;
    for (int n = 0; n < 2; n++) {
        if (adc_read_dt(&ch[i], &seq) < 0) {
            return -1;
        }
        acc += sample;
    }
    int v = (int)(acc / 2);
    return v < 0 ? 0 : v;
}

/* ---- the per-pass snapshot (controls.h) -------------------------------------
 * Channels 0..5 in ONE sequence: the SAADC converts them back to back, and
 * extra_samplings = 1 repeats the scan at once -- the two-sample average the
 * per-channel reads above take, for one driver round trip instead of twelve.
 * Every channel shares one config (gain 1/6, internal ref, 20 us acquisition,
 * 12 bit) and none oversamples, which a multi-channel sequence requires. The
 * battery (6) stays on controls_read_raw(): it is read rarely, and not here. */
#define N_SNAP 6

static int16_t snap_buf[2 * N_SNAP];   /* sampling 0 then sampling 1, channel order */
static int     snap[N_SNAP];

int controls_sample(void)
{
    static const struct adc_sequence_options opts = {
        .interval_us     = 0,          /* back to back, no timer */
        .extra_samplings = 1,
    };
    struct adc_sequence seq = {
        .options     = &opts,
        .buffer      = snap_buf,
        .buffer_size = sizeof(snap_buf),
        .resolution  = ch[0].resolution,
    };
    for (int i = 0; i < N_SNAP; i++) {
        seq.channels |= BIT(ch[i].channel_id);
    }
    int rc = adc_read(ch[0].dev, &seq);
    if (rc < 0) {
        return rc;
    }
    /* Samples land in ascending channel-id order, which is io-channels order
     * here (channel@0..5 have reg 0..5). Clamp each at 0 before averaging: the
     * single-channel path got that from the driver for every read. */
    for (int i = 0; i < N_SNAP; i++) {
        int a = snap_buf[i], b = snap_buf[N_SNAP + i];
        snap[i] = ((a < 0 ? 0 : a) + (b < 0 ? 0 : b)) / 2;
    }
    return 0;
}

int controls_snap(int idx)
{
    return (idx >= 0 && idx < N_SNAP) ? snap[idx] : -1;
}
