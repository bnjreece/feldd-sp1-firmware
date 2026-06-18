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

/*
 * Run the SAADC offset calibration once at startup. The nRF SAADC drifts with
 * temperature/supply, and adc_read_dt() with seq.calibrate left false never
 * triggers nrfx_saadc_offset_calibrate(), so the converted values quietly skew
 * over a session. A single calibrated read (seq.calibrate = true) kicks the
 * driver's offset-calibration path; the offset is instance-global, so one read
 * recalibrates the whole SAADC. We do it here, after BTN_COM is up and the
 * channels are configured, so calibration runs against the real rail.
 */
static void controls_calibrate(void)
{
    for (int i = 0; i < N_CH; i++) {
        struct adc_sequence seq = {
            .buffer      = &sample,
            .buffer_size = sizeof(sample),
            .calibrate   = true,
        };
        if (adc_sequence_init_dt(&ch[i], &seq) < 0) {
            continue;
        }
        (void)adc_read_dt(&ch[i], &seq);
    }
}

int controls_init(void)
{
    nrf_gpio_cfg_output(SP1_BTN_COM);
    nrf_gpio_pin_set(SP1_BTN_COM);                  /* power the ladders/faders */
    for (int i = 0; i < N_CH; i++) {
        if (device_is_ready(ch[i].dev)) {
            adc_channel_setup_dt(&ch[i]);
        }
    }
    controls_calibrate();                           /* SAADC offset calibration */
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
