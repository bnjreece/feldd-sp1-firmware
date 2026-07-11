#ifndef MIDI_RT_RING_H
#define MIDI_RT_RING_H
#include <stdint.h>
#include <stdbool.h>
/* Two-tier priority byte queue for the TRS MIDI TX. Real-time bytes
 * (Clock 0xF8 .. Stop 0xFC) go on a separate high-priority ring that
 * drains BEFORE the normal ring, so a clock tick jumps ahead of queued
 * CC/note bytes and is not delayed by fader bursts. Each tier is its own
 * power-of-two ring (8 and 128) with head/tail masking; a ring is full
 * when advancing tail would meet head, so capacity is size-1 (one slot
 * kept open). put returns false when full without corrupting the ring.
 * Pure; host-testable. */
struct midi_rt_ring {
    uint8_t rt[8];   uint8_t rt_h, rt_t;   // priority (real-time) ring
    uint8_t nb[128]; uint8_t nb_h, nb_t;   // normal ring
};

void midi_rt_ring_init(struct midi_rt_ring *r);
bool midi_rt_put_rt(struct midi_rt_ring *r, uint8_t b);  // enqueue a PRIORITY byte
bool midi_rt_put(struct midi_rt_ring *r, uint8_t b);     // enqueue a NORMAL byte
/* Dequeue next byte to transmit: real-time ring first (FIFO), then normal
 * (FIFO). Returns false (leaves *out untouched) if both are empty. */
bool midi_rt_next(struct midi_rt_ring *r, uint8_t *out);
#endif
