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

// Returns index of matched level, or -1 (idle / between plateaus).
int ladder_decode(uint16_t raw12, const uint16_t *levels, size_t n, uint16_t tol);

#endif
