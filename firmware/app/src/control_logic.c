#include "control_logic.h"

// 12-bit counts; ~0.5 of one 7-bit step. Tradeoff: a fader released 1-15 counts
// past a step boundary may rest one CC step off until the next move; accepted
// for zero messages at rest.
#define FADER_HYSTERESIS 16

// rail bands = full top/bottom CC step, so extremes stay reachable even if
// the ADC never reads true 0/4095 (pot end resistance, reference tolerance)
#define RAIL_LO 31
#define RAIL_HI 4064

int fader_update(fader_t *f, uint16_t raw12)
{
    if (raw12 > 4095) raw12 = 4095;
    if (f->initialized) {
        int delta = (int)raw12 - (int)f->last_sent_raw;
        if (delta < 0) delta = -delta;
        uint8_t new_cc = (uint8_t)(raw12 >> 5);             // 4096/32 = 128 steps
        uint8_t old_cc = (uint8_t)(f->last_sent_raw >> 5);
        // require crossing into a new CC step AND clearing the jitter window;
        // readings in a rail band bypass the jitter window (duplicate
        // suppression above still prevents spam inside the band)
        if (new_cc == old_cc) return -1;
        if (delta < FADER_HYSTERESIS && raw12 > RAIL_LO && raw12 < RAIL_HI) return -1;
    }
    f->initialized = 1;
    f->last_sent_raw = raw12;
    return raw12 >> 5;
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
