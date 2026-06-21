#ifndef CHORD_ENGINE_H
#define CHORD_ENGINE_H
#include <stdint.h>
#include "profile.h"

/* Depth bands sampled once per press from the chord-depth fader's last CC. */
enum chord_depth { CHORD_DEPTH_TRIAD=0, CHORD_DEPTH_7=1, CHORD_DEPTH_9=2,
                   CHORD_DEPTH_11=3, CHORD_DEPTH_13=4 };

/* Map a 0..127 chord-depth CC to a depth band (Section 8.3 thresholds). */
int chord_depth_from_cc(int cc);

/* Resolve a chord_def to a concrete low-to-high note list. `depth` is the
 * chord_depth band, applied ONLY when c->mode==2 (root+quality) AND the quality
 * is depth-eligible (1..5); explicit sets, ranges, and dim/aug/sus emit their
 * authored notes verbatim. Writes up to MAX_CHORD notes into out[], returns the
 * count (0..MAX_CHORD). out MUST have room for MAX_CHORD. Notes that would
 * exceed 127 are dropped (high-root clamp). NULL c -> 0. */
#define MAX_CHORD 8
int chord_resolve(const struct chord_def *c, int depth, uint8_t out[MAX_CHORD]);

/* Bounded MIDI-out deferral ring + per-tick cap (Section 6.4). The two-ladder
 * worst case is up to 24 messages in one tick; emitting them synchronously over
 * TRS (~960 us each) would blow the 8 ms tick. We cap at CHORD_TX_BUDGET msgs per
 * tick and spill the surplus into a small ring, draining it (Note-Offs first) at
 * the TOP of each tick. Pure: stores (status,d1,d2) triples; main.c maps each to
 * a midi_msg + midi_out_send. */
#define CHORD_TX_BUDGET 8
#define CHORD_TX_RING   32

struct chord_tx_msg { uint8_t status, d1, d2; };
struct chord_tx_ring {
    struct chord_tx_msg buf[CHORD_TX_RING];
    uint8_t head, tail, count;   /* count = pending messages */
};

void chord_tx_init(struct chord_tx_ring *r);
/* Enqueue a message. is_off!=0 marks a Note-Off so the drain can prioritize it.
 * Returns 1 if queued, 0 if the ring was full (dropped - should never happen for
 * the documented worst case of 24 < 32). */
int  chord_tx_push(struct chord_tx_ring *r, uint8_t status, uint8_t d1, uint8_t d2);
/* Pop up to CHORD_TX_BUDGET messages, Note-Offs (0x80..0x8F) first. Writes them
 * to out[] (caller emits them). Returns the number popped (0..CHORD_TX_BUDGET). */
int  chord_tx_drain(struct chord_tx_ring *r, struct chord_tx_msg out[CHORD_TX_BUDGET]);

#endif
