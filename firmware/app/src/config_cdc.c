/*
 * config_cdc.c — bind the host-tested JSON-lines config protocol (protocol.c)
 * to the USB-CDC ACM console, and emit the live monitor stream.
 *
 * Transport design (see notes/2026-06-18-cdc-bringup-saga.md for the full why):
 *
 *  - PURE POLL: no uart irq callback. We arm RX once, then read with
 *    uart_poll_in and reply with uart_poll_out from the main loop. The CDC's
 *    actual USB send runs on its OWN workqueue (CONFIG_USBD_CDC_ACM_WORKQUEUE=y
 *    in prj.conf) so it isn't starved by the USB-MIDI class on the shared system
 *    workqueue. poll_out discards (never blocks) if the tx_fifo is full.
 *  - BRACE FRAMING: the host's trailing '\n' does not arrive over this CDC, so
 *    requests are framed by the balanced top-level '}', not newlines. Requests
 *    are flat JSON; whitespace between frames is skipped.
 *  - Replies go out unconditionally; the unsolicited monitor stream is DTR-gated
 *    and rate-limited at the source (faders emit only on a CC change, buttons on
 *    edges — see main.c), so it can't flood the link.
 */
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/drivers/hwinfo.h>
#include "config_cdc.h"
#include "protocol.h"
#include "librarian.h"
#ifdef CONFIG_FELDD_BT_PROVISION
#include "bt_provision.h"   /* bt_provision_probe_ss — wired into g_store below */
#endif

/* The chosen console UART — the cdc_acm_uart0 node. usbdev_start() already
 * enumerated the composite; we read/write it here, we do NOT bring USB up. */
static const struct device *cdc;

static char g_uid[33];

static const struct proto_store g_store = {
    .read       = librarian_read,
    .write      = librarian_write,
    .set_active = librarian_set_active,
    .reset      = librarian_reset,
    .reset_all  = librarian_reset_all,
    .get_active = librarian_active_index,
    .get_mode   = librarian_mode,
    .set_mode   = librarian_set_mode,
    .get_playrole = librarian_play_mode,
    .set_playrole = librarian_set_play_mode,
    .get_midithru = librarian_midi_thru,
    .set_midithru = librarian_set_midi_thru,
    .profiles      = NUM_PROFILES,        /* GLOBAL slots 0..15 (read/write/reset) */
    .bank_profiles = NUM_BANK_PROFILES,   /* WITHIN-bank 0..7 (setactive / •• cycle) */
    .faders     = 4,
    .buttons    = 9,
    .fw         = "0.29.0-looper",
    .uid        = g_uid,
#ifdef CONFIG_FELDD_BT_PROVISION
    /* Q5 P0 stage-1 READ-ONLY SS probe. proto_handle's bt_ss_probe verb calls this; it does
     * the UART handoff from bt_link, reads the SS (no flash write), runs the gate, and returns
     * a status. The multi-second, WDT-fed call blocks config_cdc_poll (main loop) for its
     * duration — acceptable for an explicit, configurator-triggered dev probe. */
    .bt_ss_probe = bt_provision_probe_ss,
    /* Q5 P0 stage-2 DS-WRITE provisioning. proto_handle's bt_provision verb calls this; it
     * power-gates, then (only on GATE_OK + the ds_base assert) DS-only writes our BLE app to
     * the compile-time CYBT_DS_BASE_ADDR, verifies, proves the SS is untouched, and cold-boots
     * the new app. The multi-minute, WDT-fed call blocks config_cdc_poll (main loop) for its
     * duration — which is exactly what inhibits the •• power-off across the window. */
    .bt_provision = bt_provision_run,
#endif
};

/* Frame assembly. Cap from config_cdc.h (320 for v5); an overrun is dropped and
 * the parser resyncs. Requests are framed by the balanced top-level '}', string-
 * aware so braces inside "..." values are not miscounted. A v5 `write` frame is
 * ~287 bytes (240-char base64 + JSON wrapper), so the old 256 cap dropped it. */
#define LINE_CAP CONFIG_CDC_LINE_CAP
static char    g_line[LINE_CAP];
static int     g_len;
static int     g_depth;    /* JSON brace nesting depth of the current frame */
static bool    g_instr;    /* currently inside a "..." string literal */
static bool    g_escape;   /* previous char was a backslash inside a string */

/* Response scratch. Pre-v9 the worst case was the 16-entry banked list_r (1184
 * bytes: each name[16] can be all '"'/'\\', escaped to 32 wire bytes, so 16
 * entries reach 1120 + a <=61-byte header + "]}" footer; see the `list` verb
 * budget in protocol.c). v9 makes read_r the binding case: a 1038-byte profile
 * encodes to 1384-char base64, and {"t":"read_r",...,"data":"<b64>"} adds a
 * 56-char wrapper (max u32 id + 2-digit n) = 1440, + '\0' = 1441. That exceeds
 * both list_r (1184) and the old 1280 cap, so a v9 read would emit() -> -1 and
 * the host would get OVERFLOW instead of the profile. CONFIG_CDC_RESP_CAP (1536)
 * holds it with headroom. */
static char    g_resp[CONFIG_CDC_RESP_CAP];

static bool    g_mon;

/* True iff a host currently has the port open (DTR asserted). */
static bool dtr_asserted(void)
{
    uint32_t dtr = 0;
    (void)uart_line_ctrl_get(cdc, UART_LINE_CTRL_DTR, &dtr);
    return dtr != 0;
}

/* v7: public DTR state for the main loop's chord-flush-on-disconnect edge. */
int config_cdc_dtr(void){ return dtr_asserted() ? 1 : 0; }

/* Send a NUL-terminated string to the host. poll_out stores each byte and the
 * CDC workqueue does the USB send; it DISCARDS (never blocks) when the tx fifo is
 * full (flow_ctrl off). A v9 read_r frame is ~1440 bytes, far larger than the CDC
 * ACM tx ring, and spinning poll_out fills the ring faster than the K_MSEC(1)-
 * scheduled tx work can drain it, so the tail is silently dropped (a v8 read_r was
 * ~740 bytes and fit, which is why this only bit at v9). Yield every 64 bytes so
 * the tx_fifo work item runs and frees ring space. HARDWARE-ONLY bug: the host uart
 * mock never drops, so no host test catches it; validated on the SWD burner. */
static void cdc_tx(const char *s)
{
    int i = 0;
    for (const char *p = s; *p; p++) {
        uart_poll_out(cdc, (unsigned char)*p);
        if ((++i & 0x3F) == 0) {
            k_msleep(1);   /* let the CDC tx work drain a USB transfer */
        }
    }
}

/* Unsolicited monitor stream — DTR-gated so it self-throttles when no host is
 * listening (and the source rate-limits per-CC-change / per-edge in main.c). */
static void cdc_write(const char *s)
{
    if (dtr_asserted()) {
        cdc_tx(s);
    }
}

static void fill_uid(void)
{
    uint8_t raw[16];
    ssize_t n = hwinfo_get_device_id(raw, sizeof raw);
    int o = 0;
    if (n > 0) {
        static const char hex[] = "0123456789abcdef";
        for (ssize_t i = 0; i < n && o + 2 < (int)sizeof g_uid; i++) {
            g_uid[o++] = hex[(raw[i] >> 4) & 0xF];
            g_uid[o++] = hex[raw[i] & 0xF];
        }
    }
    g_uid[o] = '\0';
}

/* Dispatch one complete, NUL-terminated request line. */
static void handle_line(const char *line)
{
    struct proto_result res;
    int n = proto_handle(&g_store, line, g_resp, (int)sizeof g_resp, &res);
    if (n < 0) {
        cdc_tx("{\"t\":\"err\",\"i\":0,\"ok\":false,"
               "\"code\":\"OVERFLOW\",\"msg\":\"resp too large\"}\n");
        return;
    }

    if (res.mon_set) {
        g_mon = (res.mon_on != 0);
    }

    cdc_tx(g_resp);
    cdc_tx("\n");
}

/* Feed one received byte to the brace-framer; dispatch a complete JSON object. */
static void feed_byte(uint8_t c)
{
    if (g_len == 0 && (c == '\n' || c == '\r' || c == ' ' || c == '\t')) {
        return;  /* separator/whitespace between frames */
    }
    if (g_len >= LINE_CAP - 1) {           /* frame too long: drop + resync */
        g_len = 0; g_depth = 0; g_instr = false; g_escape = false;
        return;
    }
    g_line[g_len++] = (char)c;

    if (g_instr) {                          /* inside a "..." string literal */
        if (g_escape)       { g_escape = false; }
        else if (c == '\\') { g_escape = true; }
        else if (c == '"')  { g_instr = false; }
        return;
    }
    if (c == '"') {
        g_instr = true;
    } else if (c == '{') {
        g_depth++;
    } else if (c == '}') {
        if (g_depth > 0) {
            g_depth--;
        }
        if (g_depth == 0) {                 /* complete top-level JSON object */
            g_line[g_len] = '\0';
            handle_line(g_line);
            g_len = 0;
        }
    }
}

int config_cdc_init(void)
{
    cdc = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));
    if (!device_is_ready(cdc)) {
        return -1;
    }
    g_len    = 0;
    g_depth  = 0;
    g_instr  = false;
    g_escape = false;
    g_mon    = false;
    fill_uid();

    /* Arm the RX endpoint so uart_poll_in() has data to return; no irq callback
     * (TX/RX are poll-driven from the main loop). Assert DCD/DSR so the host
     * sees a fully "open" modem. */
    uart_irq_rx_enable(cdc);
    (void)uart_line_ctrl_set(cdc, UART_LINE_CTRL_DCD, 1);
    (void)uart_line_ctrl_set(cdc, UART_LINE_CTRL_DSR, 1);

    return 0;
}

void config_cdc_poll(void)
{
    /* Drain all available RX (uart_poll_in) and feed the brace-framer. */
    unsigned char c;
    while (uart_poll_in(cdc, &c) == 0) {
        feed_byte((uint8_t)c);
    }
}

void config_cdc_monitor_fader(int idx, int value)
{
    if (!g_mon) {
        return;
    }
    char buf[48];
    int n = snprintf(buf, sizeof buf,
                     "{\"t\":\"mon\",\"k\":\"f\",\"ix\":%d,\"v\":%d}\n",
                     idx, value);
    if (n > 0 && n < (int)sizeof buf) {
        cdc_write(buf);
    }
}

void config_cdc_monitor_button(int idx, int pressed)
{
    if (!g_mon) {
        return;
    }
    char buf[48];
    int n = snprintf(buf, sizeof buf,
                     "{\"t\":\"mon\",\"k\":\"b\",\"ix\":%d,\"s\":%d}\n",
                     idx, pressed ? 1 : 0);
    if (n > 0 && n < (int)sizeof buf) {
        cdc_write(buf);
    }
}

/* Unsolicited active-profile-changed push (on-device •• tap). Sent independent
 * of the g_mon fader/button gate so a connected host never shows a stale active
 * marker. DTR-gated poll_out. */
void config_cdc_monitor_active(int n)
{
    char buf[48];
    int len = config_cdc_fmt_active(buf, (int)sizeof buf, n);
    if (len > 0) {
        cdc_write(buf);
    }
}

/* Unsolicited mode-changed push (on-device •• + FWD/RWD flip). Sent independent
 * of the g_mon gate so a connected host's mode toggle reflects on-device flips,
 * mirroring config_cdc_monitor_active. DTR-gated poll_out. */
void config_cdc_monitor_mode(int v)
{
    char buf[48];
    int len = config_cdc_fmt_mode(buf, (int)sizeof buf, v);
    if (len > 0) {
        cdc_write(buf);
    }
}

/* Unsolicited PLAY-role push (Feature 4). Defined so the symbol resolves and a
 * future on-device gesture that flips playrole can reflect it to a connected host,
 * mirroring config_cdc_monitor_mode. DTR-gated poll_out via cdc_write. */
void config_cdc_monitor_playrole(int v)
{
    char buf[48];
    int len = config_cdc_fmt_playrole(buf, (int)sizeof buf, v);
    if (len > 0) {
        cdc_write(buf);
    }
}
