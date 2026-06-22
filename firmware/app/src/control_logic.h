#ifndef CONTROL_LOGIC_H
#define CONTROL_LOGIC_H
#include <stdint.h>
#include <stddef.h>

typedef struct {
    uint16_t last_sent_raw;   // 12-bit value at last emitted CC
    uint8_t  initialized;
} fader_t;

// Returns 0..127 CC value to send, or -1 to suppress (jitter / no change).
int fader_update(fader_t *f, uint16_t raw12);

// Like fader_update, but `rail_loaded` = a button is currently sagging the shared
// BTN_COM rail (buttons_rail_loaded()). While loaded, the jitter deadband applies
// EVERYWHERE, including the rail bands the idle path bypasses, so the persistent
// ~1-2 LSB rail droop never leaks a spurious CC; genuine moves (>> the deadband)
// still track. rail_loaded=0 is identical to fader_update. (BUG-5 / 0.15: a held
// button no longer freezes the faders, so there is no snap-to-hardware on release.)
int fader_update_rail(fader_t *f, uint16_t raw12, int rail_loaded);

// Edge-triggered droop-settle counter for the fader read gate. A button press
// sags the rail and the first couple of fader reads droop ~1-2 LSB, so the caller
// skips fader reads while the returned counter is > 0. The skip re-arms ONLY on a
// rising rail edge (rail && !rail_prev = a fresh press); a SUSTAINED hold counts
// the counter down to 0 instead of pinning it, so holding a button (e.g. a chord)
// no longer freezes the faders for the whole hold (BUG-5). Pure; the caller owns
// the `settle` and `rail_prev` state across ticks.
#define FADER_SETTLE_TICKS 2
static inline uint8_t fader_settle_step(int rail, int rail_prev, uint8_t settle) {
    if (rail && !rail_prev) return FADER_SETTLE_TICKS;   // press edge: arm the skip
    if (settle > 0) return (uint8_t)(settle - 1);        // count down (even while held)
    return 0;
}

// The quantized 0..127 CC for a 12-bit fader reading (the same rescale
// fader_update emits). Exposed so soft-takeover can compare the physical fader
// position against a stored target without going through change-suppression.
uint8_t fader_raw_to_cc(uint16_t raw12);

// Soft-takeover ("pickup"): after a shift-layer switch a fader's new mapping
// holds its previous value until the physical fader CROSSES that value, then
// resumes normal (snappy) tracking. This is NOT smoothing - it only gates WHEN a
// fader starts sending again, so a layer switch never jumps the CC.
typedef struct {
    uint8_t pending;     // 1 = suppressing emits until the fader crosses target
    uint8_t target;      // CC value to cross (the new layer's last-sent value)
    uint8_t from_above;  // 1 if the physical CC was above target when armed
} takeover_t;

// Arm a catch toward `target` given the current physical CC. If the fader is
// already at target, leaves pending=0 (nothing to catch) and returns 0.
// Otherwise sets pending=1, records which side to cross from, and returns 1.
int takeover_arm(takeover_t *t, uint8_t phys_cc, uint8_t target);

// Step one fader read. Returns 1 if the fader should EMIT now (it was not
// pending, or the physical CC just crossed the target - pending then cleared);
// returns 0 to keep suppressing.
int takeover_step(takeover_t *t, uint8_t phys_cc);

// Returns index of matched level, or -1 (idle / between plateaus).
int ladder_decode(uint16_t raw12, const uint16_t *levels, size_t n, uint16_t tol);

#endif
