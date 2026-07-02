#ifndef SP1_BOARD_H
#define SP1_BOARD_H
#include <hal/nrf_gpio.h>
#define SP1_LED1      NRF_GPIO_PIN_MAP(1,13)   /* playback LED 4 (a.k.a. side LED) */
/* The PWM3 play-LED group, pin numbers verified against the gold reference
 * stem_player-pinctrl.dtsi:79-82 (PWM_OUT0=P0.01, OUT1=P1.12, OUT2=P0.00,
 * OUT3=P1.13). SP1_LED1 above is play LED 4 (P1.13), the long-validated, already-
 * driven play LED on this board.
 *
 * The other three pins (P0.01, P1.12, P0.00) are now DRIVEN as the phase-2b mode
 * indicator (spec §7.4, USER-APPROVED): cited as PWM LEDs by the gold sp1-midi BSP
 * on this exact board, which clocks its 32 kHz off the internal RC so these pins
 * are free for LEDs. P0.00/P0.01 are confirmed SYNTH-clock GPIOs (NOT the
 * nRF52840 LF crystal) per Tim Knapen's stemplayer_pins.h — safe to drive as
 * LEDs on this unit. It is GPIO-only and Track1+4-DFU-recoverable (bootloader-level,
 * independent of the app's clock config) — NOT a brick. If the first bench flash
 * misbehaves, DFU-recover and rebuild with -DFELDD_MODE_LED_SINGLE (drives only the
 * validated P1.13 as the mode dot). The full caution lives in mode.h's HARDWARE
 * CONTRACT and the spec §7.4. */
#define SP1_PLAY_LED1 NRF_GPIO_PIN_MAP(0,1)    /* P0.01 - play LED 1 (mode indicator) */
#define SP1_PLAY_LED2 NRF_GPIO_PIN_MAP(1,12)   /* P1.12 - play LED 2 (mode indicator) */
#define SP1_PLAY_LED3 NRF_GPIO_PIN_MAP(0,0)    /* P0.00 - play LED 3 (mode indicator) */
#define SP1_PLAY_LED4 SP1_LED1                  /* P1.13 - play LED 4 (alias of SP1_LED1) */
#define SP1_FUNC_BTN  NRF_GPIO_PIN_MAP(0,27)   /* •• function, active-low pull-up */
#define SP1_BTN_COM   NRF_GPIO_PIN_MAP(1,10)   /* ladder/fader supply (used later) */
/* BQ24232 battery charger (TimK's verified pinout, via the chattock looper).
 * WITHOUT driving /CE low the battery never charges, even on USB, so the cell
 * drains flat and the device browns out at random. */
#define SP1_CHG_NCE    NRF_GPIO_PIN_MAP(0,21)  /* /CE charge-enable, active-low: low = charging on */
#define SP1_CHG_NCHG   NRF_GPIO_PIN_MAP(0,22)  /* charge status, open-drain, low = charging */
#define SP1_CHG_NPGOOD NRF_GPIO_PIN_MAP(0,24)  /* power-good,    open-drain, low = USB present */
/* Track LEDs — lit together as the "loading firmware" cue in enter_dfu(). */
#define SP1_TRACK_LED1 NRF_GPIO_PIN_MAP(0,29)
#define SP1_TRACK_LED2 NRF_GPIO_PIN_MAP(0,26)
#define SP1_TRACK_LED3 NRF_GPIO_PIN_MAP(1,15)
#define SP1_TRACK_LED4 NRF_GPIO_PIN_MAP(1,14)
#endif
