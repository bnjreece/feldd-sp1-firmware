#ifndef HELD_NOTES_H
#define HELD_NOTES_H
/*
 * held_notes.h — which notes the TRS jack is currently sounding.
 *
 * WHY. The jack can stop being a MIDI output while notes are still down: a
 * profile change can switch it to a trigger or sync role mid-phrase. Anything
 * sounding at that moment loses its route home — the Note-Off has nowhere to go —
 * and the downstream synth drones forever.
 *
 * The SP-1's own buttons are already covered: chord presses are latched and
 * released by chord_flush_all(). What is NOT covered is MIDI thru, where a whole
 * host stream is forwarded to the same jack with no tracking at all. Worse, the
 * host never learns the jack is gone: it keeps sequencing and sends its own
 * Note-Offs into what is now a pulse output, so nothing anywhere reports an error.
 *
 * So the jack tracks what it sent. On a mode flip the firmware releases exactly
 * those notes — not All Notes Off, which would kill notes the SP-1 never
 * originated (indefensible when playing into an instrument running its own
 * sequencer), and not All Sound Off, which guillotines release tails.
 *
 * A bitmap rather than a list: 16 channels x 128 notes = 2048 bits = 256 bytes,
 * bounded, no overflow policy to get wrong, O(1) to update. The update costs
 * ~12 instructions, which is ~0.02% of the 960 us it takes to transmit the
 * 3-byte message it is tracking — the bookkeeping is free; the flush is not.
 *
 * Pure and freestanding, so the host test drives the real thing.
 */
#include <stdint.h>

#define HELD_NOTES_CHANNELS 16
#define HELD_NOTES_NOTES    128
#define HELD_NOTES_BYTES    ((HELD_NOTES_CHANNELS * HELD_NOTES_NOTES) / 8)

struct held_notes {
	uint8_t bits[HELD_NOTES_BYTES];
};

static inline void held_notes_init(struct held_notes *h)
{
	for (int i = 0; i < HELD_NOTES_BYTES; i++) {
		h->bits[i] = 0;
	}
}

static inline int held_notes_is_held(const struct held_notes *h, uint8_t ch, uint8_t note)
{
	if (ch >= HELD_NOTES_CHANNELS || note >= HELD_NOTES_NOTES) {
		return 0;
	}
	unsigned idx = (unsigned)ch * HELD_NOTES_NOTES + note;
	return (h->bits[idx >> 3] >> (idx & 7u)) & 1u;
}

static inline void held_notes_set(struct held_notes *h, uint8_t ch, uint8_t note, int on)
{
	if (ch >= HELD_NOTES_CHANNELS || note >= HELD_NOTES_NOTES) {
		return;
	}
	unsigned idx  = (unsigned)ch * HELD_NOTES_NOTES + note;
	uint8_t  mask = (uint8_t)(1u << (idx & 7u));
	if (on) {
		h->bits[idx >> 3] |= mask;
	} else {
		h->bits[idx >> 3] &= (uint8_t)~mask;
	}
}

/* Fold one channel-voice message into the map.
 *
 * CALL THIS ONLY FOR A MESSAGE THAT WAS ACTUALLY ADMITTED TO THE TX RING.
 * midi_rt_put_msg() is all-or-nothing and returns false when the ring is full,
 * and with MIDI thru enabled a full ring is ordinary rather than a corner case.
 * A dropped Note-On never reached the wire, so marking it held would invent a
 * phantom note and emit a spurious Off later; a dropped Note-Off means the note
 * IS still sounding, so its bit must stay set. Both are handled by the same rule:
 * mutate only on successful admission.
 *
 * The velocity-0 rule is the other thing that bites: a Note-On with velocity 0 is
 * a Note-OFF. Treating it as an On leaves a note marked held forever, and the
 * flush then emits an Off for a note that was never sounding. */
static inline void held_notes_feed(struct held_notes *h, uint8_t status,
				   uint8_t d1, uint8_t d2)
{
	uint8_t type = (uint8_t)(status & 0xF0u);
	uint8_t ch   = (uint8_t)(status & 0x0Fu);

	if (type == 0x90u) {                 /* Note-On */
		held_notes_set(h, ch, d1, d2 != 0u);   /* velocity 0 == Note-Off */
	} else if (type == 0x80u) {          /* Note-Off */
		held_notes_set(h, ch, d1, 0);
	}
	/* Everything else — CC, pitch bend, program change, real-time — cannot
	 * leave a note sounding, so it is deliberately ignored. */
}

/* Walk the held notes. `cursor` starts at 0 and is advanced by the callee.
 * Returns 1 and fills *ch / *note while any remain, 0 when exhausted:
 *
 *   int c = 0; uint8_t ch, n;
 *   while (held_notes_next(&h, &c, &ch, &n)) { emit_note_off(ch, n); }
 *
 * Iteration does not clear anything, so a caller that fails partway can retry.
 * Skips whole zero bytes, which makes the common case (nothing held) ~8x cheaper
 * than a bit-at-a-time scan and the worst case still only 256 byte compares. */
static inline int held_notes_next(const struct held_notes *h, int *cursor,
				  uint8_t *ch, uint8_t *note)
{
	int idx = *cursor;
	while (idx < HELD_NOTES_CHANNELS * HELD_NOTES_NOTES) {
		if ((idx & 7) == 0 && h->bits[idx >> 3] == 0u) {
			idx += 8;                     /* skip an empty byte whole */
			continue;
		}
		if ((h->bits[idx >> 3] >> (idx & 7)) & 1u) {
			*ch     = (uint8_t)(idx / HELD_NOTES_NOTES);
			*note   = (uint8_t)(idx % HELD_NOTES_NOTES);
			*cursor = idx + 1;
			return 1;
		}
		idx++;
	}
	*cursor = idx;
	return 0;
}

static inline int held_notes_count(const struct held_notes *h)
{
	int c = 0, cursor = 0;
	uint8_t ch, note;
	while (held_notes_next(h, &cursor, &ch, &note)) {
		c++;
	}
	return c;
}

#endif /* HELD_NOTES_H */
