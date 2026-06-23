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
    .profiles      = NUM_PROFILES,        /* GLOBAL slots 0..15 (read/write/reset) */
    .bank_profiles = NUM_BANK_PROFILES,   /* WITHIN-bank 0..7 (setactive / •• cycle) */
    .faders     = 4,
    .buttons    = 9,
    .fw         = "0.16.0-beta",
    .uid        = g_uid,
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

/* Response scratch. The largest response is the 16-entry banked list_r. Its
 * WORST case is 1184 bytes: each name[16] can be all '"'/'\\', which the
 * sanitizer escapes to 32 wire bytes (NOT 16), so 16 entries reach 1120, plus a
 * <=61-byte header and "]}" footer (see the budget comment on the `list` verb in
 * protocol.c). 1280 (>= 1184) holds it; the prior 768 OVERFLOWED on legal input
 * (proto_handle returned -1, blocking enumeration). */
static char    g_resp[1280];

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
 * CDC workqueue does the USB send; discards (never blocks) if the fifo is full. */
static void cdc_tx(const char *s)
{
    for (const char *p = s; *p; p++) {
        uart_poll_out(cdc, (unsigned char)*p);
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
