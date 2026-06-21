/* config_cdc.h */
#ifndef CONFIG_CDC_H
#define CONFIG_CDC_H

/* Inbound RX line buffer cap (config_cdc.c). Must hold the LONGEST request frame:
 * a v7 `write` = JSON wrapper (~40 chars) + 592-char base64 (444-byte profile) +
 * '\0' ~= 633. v6's 392-char base64 fit in 480; v7's 150-byte tail pushes the
 * frame to ~633, so a v7 write would be silently dropped + resynced at 480. 768
 * covers 592 + wrapper + headroom (and the K=16 720-char variant if K is raised).
 * g_line[LINE_CAP] grows 288 B of static RAM - trivial on the nRF52840's 256 KB. */
#define CONFIG_CDC_LINE_CAP 768

int  config_cdc_init(void);                       /* bind the CDC uart, start RX */
void config_cdc_poll(void);                       /* call from main loop: drain RX, dispatch lines */
void config_cdc_monitor_fader(int idx, int value);/* if monitoring on, emit a mon line */
void config_cdc_monitor_button(int idx, int pressed);
void config_cdc_monitor_active(int n);            /* ALWAYS emit active-changed mon line (independent of monitor gate) */
void config_cdc_monitor_mode(int v);              /* ALWAYS emit a mode-changed mon line (independent of monitor gate) */
int  config_cdc_fmt_mode(char *buf, int cap, int v);
int  config_cdc_dtr(void);   /* 1 if a host has the port open (DTR asserted), else 0 */

/* Pure, host-testable formatter for the active-changed monitor frame. Writes
 * {"t":"mon","k":"active","n":<n>}\n (newline-terminated, NUL-terminated) into
 * buf[0..cap-1]. Returns the length written (excluding the NUL, including the
 * '\n'), or -1 if it would not fit. No Zephyr deps; lives in config_cdc_fmt.c. */
int  config_cdc_fmt_active(char *buf, int cap, int n);
#endif
