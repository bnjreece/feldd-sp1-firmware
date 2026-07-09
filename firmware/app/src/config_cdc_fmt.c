/*
 * config_cdc_fmt.c - pure, freestanding formatters for the CDC monitor frames.
 *
 * Split out of config_cdc.c (which is Zephyr UART glue and not host-buildable)
 * so the exact on-wire JSON-line bytes are unit-testable on the host
 * (test_config_cdc.c). config_cdc.c calls these, then cdc_write()s the result.
 * No malloc, no Zephyr: <stdio.h> snprintf only, like protocol.c's emitters.
 */
#include <stdio.h>
#include "config_cdc.h"
#include "lib_header.h"   /* LIB_HEADER_MODE_MAX — the reserved-personality ceiling */

int config_cdc_fmt_active(char *buf, int cap, int n)
{
    int len = snprintf(buf, (size_t)(cap > 0 ? cap : 0),
                       "{\"t\":\"mon\",\"k\":\"active\",\"n\":%d}\n", n);
    if (len < 0 || len >= cap) {
        return -1;   /* truncated / error -> overflow, never a partial frame */
    }
    return len;
}

int config_cdc_fmt_mode(char *buf, int cap, int v)
{
    /* Emit the RAW mode byte, mirroring protocol.c's solicited mode_r reply
     * ("\"v\":%u" on get_mode()). Do NOT boolean-collapse to (v ? 1 : 0): the
     * lib_header.h / mode.h design preserves the raw value (MIDI=0, KEYBOARD=1,
     * 2/3 reserved personalities) so a future personality isn't silently pushed
     * as KEYBOARD. An out-of-range value clamps to 0 (MIDI), matching
     * lib_header_set_mode() and mode_led_pattern()'s defensive clamp, so the two
     * channels (solicited reply + unsolicited push) never diverge. */
    int mode = (v >= 0 && v <= (int)LIB_HEADER_MODE_MAX) ? v : 0;
    int len = snprintf(buf, (size_t)(cap > 0 ? cap : 0),
                       "{\"t\":\"mon\",\"k\":\"mode\",\"v\":%d}\n", mode);
    if (len < 0 || len >= cap) {
        return -1;
    }
    return len;
}

int config_cdc_fmt_playrole(char *buf, int cap, int v)
{
    int role = (v == 1) ? 1 : 0;   /* clamp to 0/1 like fmt_mode's defensive clamp */
    int len = snprintf(buf, (size_t)(cap > 0 ? cap : 0),
                       "{\"t\":\"mon\",\"k\":\"playrole\",\"v\":%d}\n", role);
    if (len < 0 || len >= cap) {
        return -1;
    }
    return len;
}
