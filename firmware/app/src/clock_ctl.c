/* clock_ctl.c - pure clock-source mode selector (thru vs gen). See clock_ctl.h.
 * No Zephyr, no hardware: the caller supplies microsecond timestamps. */
#include "clock_ctl.h"

/* Master is declared gone after this much silence (us). Strict >: exactly at the
 * boundary still counts as present. */
#define CLOCK_CTL_GAP_US 2000000u

void clock_ctl_init(struct clock_ctl *c)
{
	c->mode        = CLK_MODE_GEN;
	c->ext_present = 0;
	c->last_ext_us = 0;
	c->prev_ext_us = 0;
	c->ext_ivl_us  = 0;
	c->have_prev   = 0;
}

void clock_ctl_ext_clock(struct clock_ctl *c, uint32_t now_us)
{
	c->ext_present = 1;
	c->mode        = CLK_MODE_THRU;
	c->last_ext_us = now_us;
	if (c->have_prev) {
		/* wrap-safe unsigned delta: measured tick interval */
		c->ext_ivl_us = now_us - c->prev_ext_us;
	}
	c->prev_ext_us = now_us;
	c->have_prev   = 1;
}

enum clock_mode clock_ctl_service(struct clock_ctl *c, uint32_t now_us)
{
	if (c->ext_present && (now_us - c->last_ext_us) > CLOCK_CTL_GAP_US) {
		/* master vanished: fall back to GEN, keep ext_ivl_us for tempo handoff */
		c->ext_present = 0;
		c->mode        = CLK_MODE_GEN;
	}
	return c->mode;
}

uint32_t clock_ctl_ext_ivl_us(const struct clock_ctl *c)
{
	return c->ext_ivl_us;
}
