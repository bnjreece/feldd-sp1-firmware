/* config_cdc.h */
#ifndef CONFIG_CDC_H
#define CONFIG_CDC_H

/* Inbound RX line buffer cap (config_cdc.c). Must hold the LONGEST request frame.
 * v9 (PROFILE_VERSION 9) is a 1038-byte profile -> 1384-char base64, so a `write`
 * frame is 1384 + a 45-char JSON wrapper = 1429, + '\0' = 1430. The old 768 cap
 * (sized for the v7 592-char frame) silently drops a v9 write and resyncs, so a
 * bench flash "succeeds" yet stores nothing. 1536 covers 1430 with headroom.
 * g_line[LINE_CAP] costs 1536 B of static RAM - trivial on the nRF52840's 256 KB. */
#define CONFIG_CDC_LINE_CAP 1536

/* Outbound response scratch cap (config_cdc.c g_resp[]). Must hold the LONGEST
 * reply frame. At v9 that is read_r: 1384-char base64 profile + 56-char wrapper =
 * 1440, + '\0' = 1441, which now exceeds the pre-v9 list_r worst case (1184).
 * 1536 covers it with headroom. */
#define CONFIG_CDC_RESP_CAP 1536

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
