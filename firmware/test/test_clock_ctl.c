#include <assert.h>
#include <stdio.h>
#include "../src/clock_ctl.h"

/* Timeout boundary constant, mirrored from the spec (>2,000,000 us = master gone). */
#define GAP_US 2000000u

static void test_init_defaults(void) {
	struct clock_ctl c;
	clock_ctl_init(&c);
	assert(c.mode == CLK_MODE_GEN);
	assert(c.ext_present == 0);
	assert(c.last_ext_us == 0);
	assert(c.prev_ext_us == 0);
	assert(c.ext_ivl_us == 0);
	assert(c.have_prev == 0);
	assert(clock_ctl_ext_ivl_us(&c) == 0);
}

static void test_first_tick_goes_thru(void) {
	struct clock_ctl c;
	clock_ctl_init(&c);
	/* first 0xF8 -> THRU, present, no interval yet (needs two ticks) */
	clock_ctl_ext_clock(&c, 0);
	assert(c.mode == CLK_MODE_THRU);
	assert(c.ext_present == 1);
	assert(c.have_prev == 1);
	assert(clock_ctl_ext_ivl_us(&c) == 0);
}

static void test_interval_measured(void) {
	struct clock_ctl c;
	clock_ctl_init(&c);
	clock_ctl_ext_clock(&c, 0);
	/* 20833 us ~= 120 BPM at 24 PPQN */
	clock_ctl_ext_clock(&c, 20833);
	assert(c.mode == CLK_MODE_THRU);
	assert(clock_ctl_ext_ivl_us(&c) == 20833);
	assert(c.last_ext_us == 20833);
}

static void test_service_holds_thru_within_gap(void) {
	struct clock_ctl c;
	clock_ctl_init(&c);
	clock_ctl_ext_clock(&c, 0);
	clock_ctl_ext_clock(&c, 20833);
	/* same instant as last tick -> still THRU */
	assert(clock_ctl_service(&c, 20833) == CLK_MODE_THRU);
	assert(c.ext_present == 1);
	/* exactly at the 2 s boundary is NOT a timeout (> is strict) */
	assert(clock_ctl_service(&c, 20833 + GAP_US) == CLK_MODE_THRU);
	assert(c.ext_present == 1);
}

static void test_service_falls_back_to_gen(void) {
	struct clock_ctl c;
	clock_ctl_init(&c);
	clock_ctl_ext_clock(&c, 0);
	clock_ctl_ext_clock(&c, 20833);
	/* one microsecond past the boundary -> master gone -> GEN */
	assert(clock_ctl_service(&c, 20833 + GAP_US + 1) == CLK_MODE_GEN);
	assert(c.mode == CLK_MODE_GEN);
	assert(c.ext_present == 0);
	/* tempo kept for a seamless handoff to the generator */
	assert(clock_ctl_ext_ivl_us(&c) == 20833);
}

static void test_recovers_to_thru(void) {
	struct clock_ctl c;
	clock_ctl_init(&c);
	clock_ctl_ext_clock(&c, 0);
	clock_ctl_ext_clock(&c, 20833);
	assert(clock_ctl_service(&c, 20833 + GAP_US + 1) == CLK_MODE_GEN);
	/* master returns -> back to THRU */
	clock_ctl_ext_clock(&c, 5000000);
	assert(c.mode == CLK_MODE_THRU);
	assert(c.ext_present == 1);
	/* service right after recovery keeps THRU */
	assert(clock_ctl_service(&c, 5000001) == CLK_MODE_THRU);
}

static void test_single_tick_then_gap(void) {
	struct clock_ctl c;
	clock_ctl_init(&c);
	/* a single tick then a long silence still falls back to GEN */
	clock_ctl_ext_clock(&c, 1000000);
	assert(c.mode == CLK_MODE_THRU);
	assert(clock_ctl_service(&c, 1000000 + GAP_US + 1) == CLK_MODE_GEN);
	assert(c.ext_present == 0);
	/* never saw a second tick, so no interval was measured */
	assert(clock_ctl_ext_ivl_us(&c) == 0);
}

static void test_service_noop_before_any_tick(void) {
	struct clock_ctl c;
	clock_ctl_init(&c);
	/* no external clock ever heard: service stays GEN, never times out */
	assert(clock_ctl_service(&c, 9000000) == CLK_MODE_GEN);
	assert(c.ext_present == 0);
	assert(c.mode == CLK_MODE_GEN);
}

int main(void) {
	test_init_defaults();
	test_first_tick_goes_thru();
	test_interval_measured();
	test_service_holds_thru_within_gap();
	test_service_falls_back_to_gen();
	test_recovers_to_thru();
	test_single_tick_then_gap();
	test_service_noop_before_any_tick();
	printf("all clock_ctl tests passed\n");
	return 0;
}
