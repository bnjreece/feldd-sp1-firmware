#include "control_logic.h"

// 12-bit counts; ~0.5 of one 7-bit step. Tradeoff: a fader released 1-15 counts
// past a step boundary may rest one CC step off until the next move; accepted
// for zero messages at rest.
#define FADER_HYSTERESIS 16

// Rail-sag COMPENSATION (0.26, petercolombo). The faders are powered by the same
// BTN_COM rail every button sags on press, and the read is ratiometric against the
// internal reference, so a held button drops every fader's raw PROPORTIONALLY to its
// position and to that button's ladder current. Peter measured the dip at CC ~100
// (raw ~2881): PLAY/VolUp (rail class 2) = -2 CC (~57 counts ~= 2.0%), the lighter
// buttons (class 1) = -1 CC (~28 counts ~= 1.0%). We ADD that droop back on the fader
// read while a button is held, so the CC never dips AND fine 1-CC moves still pass
// (a deadband can't do both). Per-mille of the raw, indexed by rail class. TUNABLE:
// if Peter still sees a residual after 0.26, nudge these (the model is pure-proportional).
static const uint16_t FADER_RAIL_SAG_PERMILLE[3] = { 0, 10, 20 };

uint16_t fader_rail_compensate(uint16_t raw12, int rail_class)
{
    if (raw12 > 4095) raw12 = 4095;
    if (rail_class < 0) rail_class = 0;
    if (rail_class > 2) rail_class = 2;
    uint32_t comp = (uint32_t)raw12
                  + ((uint32_t)raw12 * FADER_RAIL_SAG_PERMILLE[rail_class]) / 1000u;
    return (comp > 4095u) ? 4095u : (uint16_t)comp;
}

// The SP-1 faders never reach the ADC rails: pot end-resistance + the internal
// (non-ratiometric) SAADC reference mean full travel reads ~raw 0..3680, which a
// plain raw>>5 caps at CC 115 (petercolombo + benjamin, real units, 2026-06-18:
// "0-115 across the board"). raw_to_cc rescales the usable span to the full
// 0..127 so the extremes are actually reachable. The dead-bands (raw <= MIN -> 0,
// raw >= MAX -> 127) pin 0/127 through pot + reference tolerance between units,
// and double as the rail bands that bypass the jitter window below. MAX sits a
// hair under the observed ~3680 top so every unit clears 127.
#define FADER_RAW_MIN 32
#define FADER_RAW_MAX 3650

uint8_t fader_raw_to_cc(uint16_t raw)
{
    if (raw <= FADER_RAW_MIN) return 0;
    if (raw >= FADER_RAW_MAX) return 127;
    return (uint8_t)(((uint32_t)(raw - FADER_RAW_MIN) * 127u)
                     / (FADER_RAW_MAX - FADER_RAW_MIN));
}

int fader_update_rail(fader_t *f, uint16_t raw12, int rail_loaded)
{
    if (raw12 > 4095) raw12 = 4095;
    if (f->initialized) {
        int delta = (int)raw12 - (int)f->last_sent_raw;
        if (delta < 0) delta = -delta;
        uint8_t new_cc = fader_raw_to_cc(raw12);
        uint8_t old_cc = fader_raw_to_cc(f->last_sent_raw);
        // require crossing into a new CC step AND clearing the jitter window
        if (new_cc == old_cc) return -1;
        if (rail_loaded) {
            // a button is sagging the shared rail. 0.26 COMPENSATES the droop upstream
            // (fader_rail_compensate, applied by the caller), so raw12 is already
            // de-sagged here; keep the standard deadband EVERYWHERE (incl. the rail
            // bands the idle path bypasses) purely as the residual net for any
            // compensation error. A genuine move is >> FADER_HYSTERESIS and still passes.
            if (delta < FADER_HYSTERESIS) return -1;
        } else if (delta < FADER_HYSTERESIS && raw12 > FADER_RAW_MIN && raw12 < FADER_RAW_MAX) {
            // idle: readings in a rail band (<= MIN / >= MAX) bypass the jitter
            // window so 0 and 127 stay reachable from a sub-hysteresis nudge (the
            // duplicate suppression above still prevents spam inside a band).
            return -1;
        }
    }
    f->initialized = 1;
    f->last_sent_raw = raw12;
    return fader_raw_to_cc(raw12);
}

int fader_update(fader_t *f, uint16_t raw12)
{
    return fader_update_rail(f, raw12, 0);
}

int takeover_arm(takeover_t *t, uint8_t phys_cc, uint8_t target)
{
    if (phys_cc == target) {
        t->pending = 0;
        return 0;
    }
    t->pending = 1;
    t->target = target;
    t->from_above = (phys_cc > target) ? 1 : 0;
    return 1;
}

int takeover_step(takeover_t *t, uint8_t phys_cc)
{
    if (!t->pending) {
        return 1;   // not catching: emit normally
    }
    // crossed once the physical CC reaches/passes the target from the armed side
    int crossed = t->from_above ? (phys_cc <= t->target)
                                : (phys_cc >= t->target);
    if (crossed) {
        t->pending = 0;
        return 1;   // caught: resume emitting
    }
    return 0;       // still suppressing
}

int ladder_decode(uint16_t raw12, const uint16_t *levels, size_t n, uint16_t tol)
{
    for (size_t i = 0; i < n; i++) {
        int d = (int)raw12 - (int)levels[i];
        if (d < 0) d = -d;
        if (d <= tol) return (int)i;
    }
    return -1;
}
