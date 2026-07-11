#ifndef CLOCK_ROUTER_H
#define CLOCK_ROUTER_H
#include <stdint.h>
#include <stdbool.h>
#include "clock_cfg.h"
/*
 * clock_router - runtime coordinator for the MIDI clock feature: picks GEN vs THRU
 * and guarantees the design tenet "never two clocks".
 *
 * An external real-time byte arriving on USB-in (clock_router_ext_rt, called from
 * the USB-MIDI class OUT-completion callback) is forwarded to the TRS out; for a
 * clock byte (0xF8) it ALSO immediately silences the internal generator and feeds
 * the tested clock_ctl FSM (which measures the tick interval for a seamless BPM
 * handoff). clock_router_service (called every main-loop tick) ages out a vanished
 * master (>2 s with no ticks) and resumes the internal generator at the last
 * measured tempo. The generator (clock_timer) free-runs throughout; its ISR emit is
 * gated via clock_timer_set_gen_emit so a mode flip never races a start/stop. The
 * two entry points run in different contexts (USB callback vs main loop) and share
 * the FSM under a short irq_lock.
 */
void clock_router_init(void);          /* reset the FSM to GEN; call after clock_timer_init */
void clock_router_ext_rt(uint8_t rt);  /* a real-time byte from USB-in: forward + THRU/detect */
void clock_router_service(void);       /* every main-loop tick: age-out master + GEN fallback */

/* Apply the ACTIVE profile's clock config: `enable` gates the whole feature (GEN +
 * THRU), and the tap-button / BPM-fader assignments are latched for the control
 * scan to consult. Tempo: on a profile SWITCH (is_boot=false) the clock loads that
 * profile's default_bpm; on BOOT (is_boot=true) it keeps the restored global tempo,
 * so a power cycle resumes the last-used tempo rather than snapping to a default.
 * Call at boot and on every active-profile change. */
void clock_router_apply_profile(const struct clock_cfg *cfg, bool is_boot,
                                uint16_t global_bpm);

bool    clock_router_enabled(void);     /* is the active profile's clock on */
uint8_t clock_router_tap_button(void);  /* assigned tap button 0..8, or CLOCK_TAP_NONE */
uint8_t clock_router_bpm_fader(void);   /* assigned BPM fader 0..3, or CLOCK_FADER_NONE */

#endif /* CLOCK_ROUTER_H */
