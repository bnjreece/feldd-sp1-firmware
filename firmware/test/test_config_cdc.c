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

int main(void)
{
    t_fmt_active_basic();
    t_fmt_active_indices();
    t_fmt_active_overflow();
    printf("all config_cdc tests passed\n");
    return 0;
}
