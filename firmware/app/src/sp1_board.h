#ifndef SP1_BOARD_H
#define SP1_BOARD_H
#include <hal/nrf_gpio.h>
#define SP1_LED1      NRF_GPIO_PIN_MAP(1,13)   /* playback LED 1 */
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
