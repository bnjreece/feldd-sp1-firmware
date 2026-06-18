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

int config_cdc_fmt_active(char *buf, int cap, int n)
{
    int len = snprintf(buf, (size_t)(cap > 0 ? cap : 0),
                       "{\"t\":\"mon\",\"k\":\"active\",\"n\":%d}\n", n);
    if (len < 0 || len >= cap) {
        return -1;   /* truncated / error -> overflow, never a partial frame */
    }
    return len;
}
