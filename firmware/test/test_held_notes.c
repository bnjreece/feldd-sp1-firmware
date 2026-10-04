/*
 * test_held_notes.c — host test for held_notes.h.
 *
 * Drives the real inline functions. The cases here are the ones that would
 * otherwise be discovered as a drone from a synth: the velocity-0 rule, and the
 * requirement that a message dropped by a full ring must not mutate the map.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "held_notes.h"

static int checks;
#define CHECK(c) do { assert(c); checks++; } while (0)

static void t_starts_empty(void)
{
	struct held_notes h;
	held_notes_init(&h);
	CHECK(held_notes_count(&h) == 0);
	for (uint8_t c = 0; c < 16; c++) {
		CHECK(!held_notes_is_held(&h, c, 60));
	}
}

static void t_note_on_then_off(void)
{
	struct held_notes h;
	held_notes_init(&h);

	held_notes_feed(&h, 0x92, 60, 100);          /* ch 3 (0-based 2), note 60 */
	CHECK(held_notes_is_held(&h, 2, 60));
	CHECK(held_notes_count(&h) == 1);

	held_notes_feed(&h, 0x82, 60, 0);            /* Note-Off, same note */
	CHECK(!held_notes_is_held(&h, 2, 60));
	CHECK(held_notes_count(&h) == 0);
}

/* THE rule that bites: a Note-On with velocity 0 is a Note-OFF. Treating it as an
 * On leaves the note marked held forever, and the flush then emits an Off for a
 * note that was never sounding. */
static void t_note_on_velocity_zero_is_note_off(void)
{
	struct held_notes h;
	held_notes_init(&h);

	held_notes_feed(&h, 0x90, 64, 127);
	CHECK(held_notes_is_held(&h, 0, 64));

	held_notes_feed(&h, 0x90, 64, 0);            /* Note-On, velocity 0 */
	CHECK(!held_notes_is_held(&h, 0, 64));
	CHECK(held_notes_count(&h) == 0);
}

static void t_channels_are_independent(void)
{
	struct held_notes h;
	held_notes_init(&h);

	held_notes_feed(&h, 0x90, 60, 100);          /* ch 0 */
	held_notes_feed(&h, 0x9F, 60, 100);          /* ch 15, same note */
	CHECK(held_notes_is_held(&h, 0,  60));
	CHECK(held_notes_is_held(&h, 15, 60));
	CHECK(held_notes_count(&h) == 2);

	held_notes_feed(&h, 0x80, 60, 0);            /* release ch 0 only */
	CHECK(!held_notes_is_held(&h, 0,  60));
	CHECK(held_notes_is_held(&h, 15, 60));       /* ch 15 untouched */
}

static void t_non_note_messages_are_ignored(void)
{
	struct held_notes h;
	held_notes_init(&h);

	held_notes_feed(&h, 0xB0, 123, 0);           /* CC 123 (All Notes Off) */
	held_notes_feed(&h, 0xB0,   7, 100);         /* CC 7 */
	held_notes_feed(&h, 0xC0,   5, 0);           /* program change */
	held_notes_feed(&h, 0xE0,   0, 64);          /* pitch bend */
	held_notes_feed(&h, 0xF8,   0, 0);           /* clock */
	CHECK(held_notes_count(&h) == 0);

	/* A host's own CC 123 silences notes we still believe are held. We
	 * deliberately do NOT track that: a spurious Note-Off releases an already
	 * released note and is inaudible, while a missed one drones forever. Bias
	 * toward over-tracking. */
	held_notes_feed(&h, 0x90, 60, 100);
	held_notes_feed(&h, 0xB0, 123, 0);
	CHECK(held_notes_is_held(&h, 0, 60));
}

/* A message the ring refused never reached the wire. The caller must not feed it,
 * and this test documents the consequence of getting that wrong: the map would
 * disagree with reality in whichever direction the dropped message pointed. */
static void t_dropped_messages_must_not_be_fed(void)
{
	struct held_notes real, phantom;
	held_notes_init(&real);
	held_notes_init(&phantom);

	/* Correct: admission failed, so nothing is fed. */
	CHECK(held_notes_count(&real) == 0);

	/* Wrong: feeding a dropped Note-On invents a note that is not sounding. */
	held_notes_feed(&phantom, 0x90, 60, 100);
	CHECK(held_notes_count(&phantom) == 1);
	CHECK(held_notes_count(&real) != held_notes_count(&phantom));

	/* And a dropped Note-OFF must leave the bit SET, because the note really is
	 * still sounding — which is what happens naturally when it is not fed. */
	held_notes_init(&real);
	held_notes_feed(&real, 0x90, 60, 100);       /* admitted */
	/* Note-Off dropped by a full ring -> not fed -> still held */
	CHECK(held_notes_is_held(&real, 0, 60));
}

static void t_iteration_yields_every_held_note_once(void)
{
	struct held_notes h;
	held_notes_init(&h);

	held_notes_feed(&h, 0x90,   0, 1);           /* ch 0,  note 0   — first bit */
	held_notes_feed(&h, 0x95,  60, 1);           /* ch 5,  note 60  */
	held_notes_feed(&h, 0x9F, 127, 1);           /* ch 15, note 127 — last bit */

	int cursor = 0, seen = 0;
	uint8_t ch, note;
	int got_first = 0, got_mid = 0, got_last = 0;
	while (held_notes_next(&h, &cursor, &ch, &note)) {
		seen++;
		if (ch == 0  && note == 0)   got_first = 1;
		if (ch == 5  && note == 60)  got_mid   = 1;
		if (ch == 15 && note == 127) got_last  = 1;
	}
	CHECK(seen == 3);
	CHECK(got_first && got_mid && got_last);

	/* iteration is non-destructive, so a caller that fails partway can retry */
	CHECK(held_notes_count(&h) == 3);
}

static void t_full_map_iterates_completely(void)
{
	struct held_notes h;
	held_notes_init(&h);
	for (int c = 0; c < 16; c++) {
		for (int n = 0; n < 128; n++) {
			held_notes_feed(&h, (uint8_t)(0x90 | c), (uint8_t)n, 100);
		}
	}
	CHECK(held_notes_count(&h) == 16 * 128);

	/* and clearing one channel leaves exactly the rest */
	for (int n = 0; n < 128; n++) {
		held_notes_feed(&h, 0x83, (uint8_t)n, 0);
	}
	CHECK(held_notes_count(&h) == 15 * 128);
}

static void t_out_of_range_is_ignored_not_corrupting(void)
{
	struct held_notes h;
	held_notes_init(&h);
	held_notes_set(&h, 16, 60, 1);               /* channel out of range */
	held_notes_set(&h, 0, 128, 1);               /* note out of range */
	CHECK(held_notes_count(&h) == 0);
	CHECK(!held_notes_is_held(&h, 16, 60));
	CHECK(!held_notes_is_held(&h, 0, 128));
}

static void t_size_is_the_documented_256_bytes(void)
{
	CHECK(sizeof(struct held_notes) == 256);
	CHECK(HELD_NOTES_BYTES == 256);
}

int main(void)
{
	t_starts_empty();
	t_note_on_then_off();
	t_note_on_velocity_zero_is_note_off();
	t_channels_are_independent();
	t_non_note_messages_are_ignored();
	t_dropped_messages_must_not_be_fed();
	t_iteration_yields_every_held_note_once();
	t_full_map_iterates_completely();
	t_out_of_range_is_ignored_not_corrupting();
	t_size_is_the_documented_256_bytes();
	printf("all held_notes tests passed (%d checks)\n", checks);
	return 0;
}
