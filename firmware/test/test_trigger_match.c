/* test_trigger_match.c — host tests for the pure trigger note-match decision.
 * Same shape as the rest of firmware/test/: no framework, assert + a count. */
#include <assert.h>
#include <stdio.h>
#include "trigger_match.h"

static int checks;
#define CHECK(c) do { assert(c); checks++; } while (0)

/* note-on, ch 0, note 51, vel 100 */
static const uint8_t on_51[3]      = { 0x90, 51, 100 };
static const uint8_t on_51_ch5[3]  = { 0x95, 51, 100 };
static const uint8_t on_51_vel0[3] = { 0x90, 51, 0 };
static const uint8_t off_51[3]     = { 0x80, 51, 64 };
static const uint8_t on_38[3]      = { 0x90, 38, 100 };
static const uint8_t cc_51[3]      = { 0xB0, 51, 100 };
static const uint8_t pc_51[2]      = { 0xC0, 51 };

static void test_exact_match(void)
{
	struct trigger_match_cfg c = { .note = 51, .channel = 0 };

	CHECK(trigger_match(&c, on_51, 3) == 1);
	CHECK(trigger_match(&c, on_38, 3) == 0);       /* wrong note    */
	CHECK(trigger_match(&c, on_51_ch5, 3) == 0);   /* wrong channel */
}

/* The bug this whole file exists to prevent: a note-on with velocity 0 is a
 * note-OFF. Firing on it advances the sequencer twice per note. */
static void test_velocity_zero_is_note_off(void)
{
	struct trigger_match_cfg c = { .note = 51, .channel = TRIGGER_MATCH_ANY };

	CHECK(trigger_match(&c, on_51, 3) == 1);
	CHECK(trigger_match(&c, on_51_vel0, 3) == 0);
	CHECK(trigger_match(&c, off_51, 3) == 0);      /* real note-off too */
}

static void test_wildcards(void)
{
	struct trigger_match_cfg omni = { .note = 51, .channel = TRIGGER_MATCH_ANY };
	struct trigger_match_cfg anynote = { .note = TRIGGER_MATCH_ANY, .channel = 0 };
	struct trigger_match_cfg both = { .note = TRIGGER_MATCH_ANY,
					  .channel = TRIGGER_MATCH_ANY };

	CHECK(trigger_match(&omni, on_51_ch5, 3) == 1);   /* any channel */
	CHECK(trigger_match(&omni, on_38, 3) == 0);       /* still note-gated */
	CHECK(trigger_match(&anynote, on_38, 3) == 1);    /* any note */
	CHECK(trigger_match(&anynote, on_51_ch5, 3) == 0);/* still channel-gated */
	CHECK(trigger_match(&both, on_38, 3) == 1);
	CHECK(trigger_match(&both, on_51_vel0, 3) == 0);  /* wildcards don't defeat
							   * the vel-0 rule */
}

/* Only note messages fire. The same decoder emits CC, program change and
 * pitch bend, and a CC whose controller number equals the trigger note must
 * not be mistaken for it. */
static void test_other_message_types(void)
{
	struct trigger_match_cfg c = { .note = 51, .channel = TRIGGER_MATCH_ANY };

	CHECK(trigger_match(&c, cc_51, 3) == 0);
	CHECK(trigger_match(&c, pc_51, 2) == 0);
}

/* Short / malformed input must return 0 rather than read past the buffer. */
static void test_short_and_null(void)
{
	struct trigger_match_cfg c = { .note = 51, .channel = TRIGGER_MATCH_ANY };

	CHECK(trigger_match(&c, on_51, 2) == 0);
	CHECK(trigger_match(&c, on_51, 0) == 0);
	CHECK(trigger_match(&c, 0, 3) == 0);
	CHECK(trigger_match(0, on_51, 3) == 0);
}

static void test_cfg_valid(void)
{
	struct trigger_match_cfg ok      = { .note = 51,  .channel = 15 };
	struct trigger_match_cfg wild    = { .note = TRIGGER_MATCH_ANY,
					     .channel = TRIGGER_MATCH_ANY };
	struct trigger_match_cfg badnote = { .note = 128, .channel = 0 };
	struct trigger_match_cfg badchan = { .note = 51,  .channel = 16 };

	CHECK(trigger_match_cfg_valid(&ok) == 1);
	CHECK(trigger_match_cfg_valid(&wild) == 1);
	CHECK(trigger_match_cfg_valid(&badnote) == 0);
	CHECK(trigger_match_cfg_valid(&badchan) == 0);
	CHECK(trigger_match_cfg_valid(0) == 0);
}

/* The default configuration must fire on the pad it advertises. */
static void test_default_note_51(void)
{
	struct trigger_match_cfg d = { .note = 51, .channel = TRIGGER_MATCH_ANY };

	for (uint8_t ch = 0; ch < 16; ch++) {
		uint8_t msg[3] = { (uint8_t)(0x90 | ch), 51, 1 };
		CHECK(trigger_match(&d, msg, 3) == 1);
	}
	for (uint8_t n = 0; n < 128; n++) {
		uint8_t msg[3] = { 0x90, n, 100 };
		CHECK(trigger_match(&d, msg, 3) == (n == 51));
	}
}

int main(void)
{
	test_exact_match();
	test_velocity_zero_is_note_off();
	test_wildcards();
	test_other_message_types();
	test_short_and_null();
	test_cfg_valid();
	test_default_note_51();
	printf("all trigger_match tests passed (%d checks)\n", checks);
	return 0;
}
