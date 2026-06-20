/* config_cdc.h */
#ifndef CONFIG_CDC_H
#define CONFIG_CDC_H
int  config_cdc_init(void);                       /* bind the CDC uart, start RX */
void config_cdc_poll(void);                       /* call from main loop: drain RX, dispatch lines */
void config_cdc_monitor_fader(int idx, int value);/* if monitoring on, emit a mon line */
void config_cdc_monitor_button(int idx, int pressed);
void config_cdc_monitor_active(int n);            /* ALWAYS emit active-changed mon line (independent of monitor gate) */
void config_cdc_monitor_mode(int v);              /* ALWAYS emit a mode-changed mon line (independent of monitor gate) */
int  config_cdc_fmt_mode(char *buf, int cap, int v);

/* Pure, host-testable formatter for the active-changed monitor frame. Writes
 * {"t":"mon","k":"active","n":<n>}\n (newline-terminated, NUL-terminated) into
 * buf[0..cap-1]. Returns the length written (excluding the NUL, including the
 * '\n'), or -1 if it would not fit. No Zephyr deps; lives in config_cdc_fmt.c. */
int  config_cdc_fmt_active(char *buf, int cap, int n);
#endif
