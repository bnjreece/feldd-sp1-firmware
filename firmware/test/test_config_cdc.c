/*
 * test_config_cdc.c - host TDD for the pure formatting helpers in config_cdc_fmt.c.
 *
 * config_cdc.c itself is Zephyr glue (UART poll API, DTR line-control) and is
 * NOT host-buildable, so the wire-format string production is split into a pure,
 * freestanding helper (config_cdc_fmt.c) that we exercise here. The Zephyr
 * emitter (config_cdc_monitor_active) calls this helper then cdc_write()s the
 * result, so proving the bytes here proves the on-wire frame.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "config_cdc.h"

/* 1. active frame: exact bytes, trailing newline, no monitor gate. */
static void t_fmt_active_basic(void)
{
    char buf[48];
    int n = config_cdc_fmt_active(buf, (int)sizeof buf, 3);
    assert(n > 0);
    assert(strcmp(buf, "{\"t\":\"mon\",\"k\":\"active\",\"n\":3}\n") == 0);
    /* returned length counts the newline and excludes the NUL */
    assert(n == (int)strlen(buf));
}

/* 2. index 0 and a two-digit index both format correctly. */
static void t_fmt_active_indices(void)
{
    char buf[48];
    int n0 = config_cdc_fmt_active(buf, (int)sizeof buf, 0);
    assert(n0 > 0);
    assert(strcmp(buf, "{\"t\":\"mon\",\"k\":\"active\",\"n\":0}\n") == 0);

    int n7 = config_cdc_fmt_active(buf, (int)sizeof buf, 7);
    assert(n7 > 0);
    assert(strcmp(buf, "{\"t\":\"mon\",\"k\":\"active\",\"n\":7}\n") == 0);

    int n12 = config_cdc_fmt_active(buf, (int)sizeof buf, 12);
    assert(n12 > 0);
    assert(strcmp(buf, "{\"t\":\"mon\",\"k\":\"active\",\"n\":12}\n") == 0);
}

/* 3. too-small cap returns -1 and never overruns. */
static void t_fmt_active_overflow(void)
{
    char buf[8];
    int n = config_cdc_fmt_active(buf, (int)sizeof buf, 3);
    assert(n == -1);

    /* cap of 0 is tolerated (no write, -1). */
    int z = config_cdc_fmt_active(buf, 0, 3);
    assert(z == -1);
}

/* 4. mode frame: exact bytes, trailing newline. */
static void t_fmt_mode_basic(void)
{
    char buf[48];
    int n0 = config_cdc_fmt_mode(buf, (int)sizeof buf, 0);
    assert(n0 > 0);
    assert(strcmp(buf, "{\"t\":\"mon\",\"k\":\"mode\",\"v\":0}\n") == 0);

    int n1 = config_cdc_fmt_mode(buf, (int)sizeof buf, 1);
    assert(n1 > 0);
    assert(strcmp(buf, "{\"t\":\"mon\",\"k\":\"mode\",\"v\":1}\n") == 0);

    /* RAW byte, not boolean-collapsed: a reserved personality (mode 2/3) pushes
     * its real value so the unsolicited push matches the solicited mode_r reply
     * (protocol.c emits get_mode() raw via %u). A (v ? 1 : 0) coercion would
     * wrongly emit "v":1 here and diverge the two channels. */
    int n2 = config_cdc_fmt_mode(buf, (int)sizeof buf, 2);
    assert(n2 > 0);
    assert(strcmp(buf, "{\"t\":\"mon\",\"k\":\"mode\",\"v\":2}\n") == 0);

    int n3 = config_cdc_fmt_mode(buf, (int)sizeof buf, 3);
    assert(n3 > 0);
    assert(strcmp(buf, "{\"t\":\"mon\",\"k\":\"mode\",\"v\":3}\n") == 0);

    /* out-of-range clamps to 0 (MIDI), matching lib_header_set_mode()'s clamp. */
    int nbig = config_cdc_fmt_mode(buf, (int)sizeof buf, 99);
    assert(nbig > 0);
    assert(strcmp(buf, "{\"t\":\"mon\",\"k\":\"mode\",\"v\":0}\n") == 0);

    /* too-small cap -> -1, no overrun */
    char tiny[8];
    assert(config_cdc_fmt_mode(tiny, (int)sizeof tiny, 1) == -1);
}

/* 5. inbound RX line-cap budget: a v5 write frame is ~286 chars (240-char base64
 * + the JSON wrapper). The inbound RX buffer must hold it or config_cdc drops the
 * frame and v5 writes silently fail (feldd-multi-layer-v5 spec open item 1). Pin
 * the budget so a future struct growth that overflows the line is caught in CI,
 * not on a bench flash. */
static void t_line_cap_fits_v5_write(void)
{
    int v5_b64 = ((180 + 2) / 3) * 4;                /* 240 */
    int frame  = (int)sizeof("{\"t\":\"write\",\"i\":4294967295,\"n\":15,\"data\":\"\"}") - 1
                 + v5_b64;                            /* wrapper + payload */
    assert(CONFIG_CDC_LINE_CAP >= frame + 1);        /* +1 for the trailing '\0' */
    /* the old 256-byte cap was too small for the v5 frame (regression guard) */
    assert(frame + 1 > 256);
}

/* 6. inbound RX line-cap budget for v6: a v6 profile is 294 bytes -> 392-char
 * base64, so a `write` frame is ~437 chars (392 b64 + the JSON wrapper) + the
 * trailing '\0'. That overflows the old 320-byte cap (a v6 write would be silently
 * dropped and the bench flash would "succeed" yet store nothing). Pin the new
 * budget here so a future struct growth that overflows the line is caught in CI. */
static void t_line_cap_fits_v6_write(void)
{
    int v6_b64 = ((294 + 2) / 3) * 4;                /* 392 */
    int frame  = (int)sizeof("{\"t\":\"write\",\"i\":4294967295,\"n\":15,\"data\":\"\"}") - 1
                 + v6_b64;                            /* wrapper + payload */
    assert(CONFIG_CDC_LINE_CAP >= frame + 1);        /* +1 for the trailing '\0' */
    /* regression guard: the old 320-byte cap was too small for the v6 frame, so a
     * bump was REQUIRED. (v5's 240-char frame fit in 320; v6's 392-char does not.) */
    assert(frame + 1 > 320);
}

/* 7. inbound RX line-cap budget for v9: a v9 profile is 1038 bytes -> 1384-char
 * base64, so a `write` frame is 1429 chars (1384 b64 + the 45-char JSON wrapper)
 * + the trailing '\0' = 1430. That overflows the old 768-byte cap (a v9 write
 * would be silently dropped + resynced and a bench flash would "succeed" yet store
 * nothing). Pin the new budget here so a future struct growth that overflows the
 * line is caught in CI, not on a bench flash. */
static void t_line_cap_fits_v9_write(void)
{
    int v9_b64 = ((1038 + 2) / 3) * 4;               /* 1384 */
    int frame  = (int)sizeof("{\"t\":\"write\",\"i\":4294967295,\"n\":15,\"data\":\"\"}") - 1
                 + v9_b64;                            /* wrapper (45) + payload = 1429 */
    assert(CONFIG_CDC_LINE_CAP >= frame + 1);        /* +1 for the trailing '\0' */
    /* regression guard: the old 768-byte cap was too small for the v9 frame. */
    assert(frame + 1 > 768);
}

/* 8. outbound response-cap budget for v9: a v9 read_r reply carries the full
 * 1384-char base64 profile plus the {"t":"read_r","i":<u32>,"ok":true,"n":<nn>,
 * "data":"<b64>"} wrapper (56 chars with a max u32 id + 2-digit n) = 1440 chars
 * + '\0' = 1441. That exceeds the old g_resp[1280] (sized for the 1184-byte
 * 16-name list_r worst case), so a v9 read would emit() -> -1 and the host would
 * get OVERFLOW instead of the profile. Pin the response cap so a v9 read cannot
 * truncate. */
static void t_resp_cap_fits_v9_read(void)
{
    int v9_b64 = ((1038 + 2) / 3) * 4;               /* 1384 */
    int frame  = (int)sizeof("{\"t\":\"read_r\",\"i\":4294967295,\"ok\":true,\"n\":15,\"data\":\"\"}") - 1
                 + v9_b64;                            /* wrapper (56) + payload = 1440 */
    assert(CONFIG_CDC_RESP_CAP >= frame + 1);        /* +1 for the trailing '\0' */
    /* regression guard: the old 1280-byte g_resp was too small for a v9 read. */
    assert(frame + 1 > 1280);
}

/* playrole push frame: exact bytes, trailing newline; raw value 0/1; overflow. */
static void t_fmt_playrole_basic(void)
{
    char buf[64];
    assert(config_cdc_fmt_playrole(buf, (int)sizeof buf, 0) > 0);
    assert(strcmp(buf, "{\"t\":\"mon\",\"k\":\"playrole\",\"v\":0}\n") == 0);
    assert(config_cdc_fmt_playrole(buf, (int)sizeof buf, 1) > 0);
    assert(strcmp(buf, "{\"t\":\"mon\",\"k\":\"playrole\",\"v\":1}\n") == 0);
    char tiny[8];
    assert(config_cdc_fmt_playrole(tiny, (int)sizeof tiny, 1) == -1);   /* overflow */
}

int main(void)
{
    t_fmt_active_basic();
    t_fmt_playrole_basic();
    t_fmt_active_indices();
    t_fmt_active_overflow();
    t_fmt_mode_basic();
    t_line_cap_fits_v5_write();
    t_line_cap_fits_v6_write();
    t_line_cap_fits_v9_write();
    t_resp_cap_fits_v9_read();
    printf("all config_cdc tests passed\n");
    return 0;
}
