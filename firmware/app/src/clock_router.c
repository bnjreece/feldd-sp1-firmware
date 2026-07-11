/*
 * clock_router.c - GEN/THRU coordinator for the MIDI clock feature. See clock_router.h.
 */
#include "clock_router.h"
#include "clock_ctl.h"
#include "clock_timer.h"
#include "clockgen.h"
#include "midi_out.h"
#include <zephyr/kernel.h>

#define MIDI_RT_CLOCK 0xF8u

/* The clock-source FSM. Touched from two contexts: clock_router_ext_rt (USB OUT
 * callback) and clock_router_service (main loop). Both hold irq_lock across their
 * FSM access; the sections are a handful of instructions. */
static struct clock_ctl g_ctl;

/* The active profile's clock config, latched by clock_router_apply_profile. When
 * clock_on is false the feature is fully off (no GEN, no THRU forward). tap_btn /
 * bpm_fdr tell the control scan which button/fader drive the tempo. volatile: written
 * by the main thread (apply_profile), read by the higher-priority usbd thread (ext_rt)
 * and the main-loop control scan; all are single-byte, so reads/writes are atomic. */
static volatile bool    clock_on;
static volatile uint8_t tap_btn = CLOCK_TAP_NONE;
static volatile uint8_t bpm_fdr = CLOCK_FADER_NONE;

static inline uint32_t router_now_us(void)
{
    /* Microsecond-grade timestamp from the system cycle clock (~30 us on this
     * RTC-backed build) so the measured external tick interval - and thus the
     * THRU->GEN fallback tempo - tracks the generator's resolution instead of
     * quantizing to 1 ms (a ~5% tempo step at 120 BPM). uint32 us wraps at ~71 min;
     * every consumer uses modular subtraction, which stays correct across the wrap. */
    return k_cyc_to_us_floor32(k_cycle_get_32());
}

void clock_router_init(void)
{
    clock_ctl_init(&g_ctl);
}

void clock_router_ext_rt(uint8_t rt)
{
    if (!clock_on) {
        return; /* clock feature off for this profile: no THRU forward, no detect */
    }
    if (rt == MIDI_RT_CLOCK) {
        /* Silence the internal generator BEFORE forwarding the master tick so the
         * two never overlap on the wire (tenet: never two clocks). Set on EVERY
         * external tick, not just the GEN->THRU edge, so a race with the fallback
         * resume in clock_router_service self-heals within one master tick. */
        clock_timer_set_gen_emit(false);
        uint32_t t = router_now_us();
        unsigned int key = irq_lock();
        clock_ctl_ext_clock(&g_ctl, t);
        irq_unlock(key);
    }
    /* Pass the master real-time byte (clock / start / continue / stop) through to
     * the TRS out via the priority tier so it jumps ahead of any queued CC. */
    midi_out_rt(rt);
}

void clock_router_service(void)
{
    if (!clock_on) {
        return; /* no internal generator when the active profile disables the clock */
    }
    uint32_t t = router_now_us();
    unsigned int key = irq_lock();
    enum clock_mode m = clock_ctl_service(&g_ctl, t);
    uint32_t ivl = clock_ctl_ext_ivl_us(&g_ctl);
    /* Fallback edge: the master just aged out (mode fell to GEN) while the internal
     * generator is still silenced from THRU. Decide AND assert the resume INSIDE the
     * lock so a preempting clock_router_ext_rt (the usbd thread is a higher-priority
     * coop thread and can preempt this one) cannot slip a fresh THRU flip between the
     * mode read and the gen_emit write: it then either happens-before (we read THRU,
     * no resume) or happens-after (its gen_emit=false is the last write and wins). The
     * slow BPM reprogram stays OUTSIDE the lock. In steady GEN this is a no-op, so the
     * fader/tap stay in charge of the tempo. */
    bool resume = (m == CLK_MODE_GEN && !clock_timer_gen_emit());
    if (resume) {
        clock_timer_set_gen_emit(true);
    }
    irq_unlock(key);

    if (resume && ivl) {
        clock_timer_set_bpm(clockgen_us_to_bpm(ivl));
    }
}

void clock_router_apply_profile(const struct clock_cfg *cfg, bool is_boot,
                                uint16_t global_bpm)
{
    tap_btn = cfg->tap_button;
    bpm_fdr = cfg->bpm_fader;

    if (cfg->enable) {
        /* ENABLE: configure the timer + FSM, THEN set clock_on last, so a preempting
         * ext_rt (higher-priority usbd thread) only starts acting once everything is
         * ready and never sees a half-applied state. */
        uint16_t bpm = is_boot ? global_bpm : cfg->default_bpm;
        if (clock_timer_running()) {
            clock_timer_set_bpm(bpm);
        } else {
            clock_timer_start(bpm);
        }
        clock_timer_set_gen_emit(true); /* GEN by default; ext_rt flips to THRU */

        unsigned int key = irq_lock();
        clock_ctl_init(&g_ctl); /* fresh GEN baseline; guarded vs a concurrent ext_rt */
        irq_unlock(key);

        clock_on = true; /* LAST */
    } else {
        /* DISABLE: clear clock_on FIRST so a preempting ext_rt early-returns and can
         * neither forward a stray tick nor dirty the FSM we are about to reset. */
        clock_on = false;
        clock_timer_stop();

        unsigned int key = irq_lock();
        clock_ctl_init(&g_ctl);
        irq_unlock(key);
    }
}

bool clock_router_enabled(void)
{
    return clock_on;
}

uint8_t clock_router_tap_button(void)
{
    return tap_btn;
}

uint8_t clock_router_bpm_fader(void)
{
    return bpm_fdr;
}
