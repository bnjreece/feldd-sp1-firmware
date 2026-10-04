/*
 * test_profile_migrate.c — host test for profile_migrate.h.
 *
 * Drives the REAL inline functions, not a reimplementation, so deleting a step
 * (the tail zeroing, the version stamp, the ordering of the two) fails here
 * rather than on a device.
 *
 * A note on what is being tested at v9. Nothing at v9 is append-compatible, so
 * profile_migrate_src_ok() accepts no older length and the production path
 * cannot widen anything yet. The MECHANISM is still fully testable: the tests
 * below call profile_migrate_apply() directly with a synthetic older length,
 * which is exactly the split that function exists for. When a future version
 * appends and adds its predecessor to the accept list, t_src_ok_rejects_v8 and
 * t_migrate_rejects_unlisted_length are the tests that will need revisiting —
 * deliberately, not by accident.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "profile_migrate.h"

static int checks;
#define CHECK(c) do { assert(c); checks++; } while (0)

/* A synthetic "older" length: the current struct minus a 12-byte tail. Stands in
 * for a real predecessor so the widen path can be exercised before one exists. */
#define SYNTH_TAIL 12u
#define SYNTH_LEN  (sizeof(struct profile) - SYNTH_TAIL)

/* Fill callback that writes a recognisable non-zero pattern into the tail, so a
 * test can tell "the callback ran" from "the bytes happened to be zero". */
static int   fill_calls;
static int   fill_saw_len;
static void  tail_fill(struct profile *out, int src_len)
{
	fill_calls++;
	fill_saw_len = src_len;
	memset((uint8_t *)out + src_len, 0xA5, sizeof(struct profile) - (size_t)src_len);
}

/* A callback that (wrongly) stamps a bogus version, to prove the real stamp
 * happens AFTER the fill and therefore wins. */
static void bad_fill(struct profile *out, int src_len)
{
	(void)src_len;
	out->version = 0x7F;
}

static void mk_src(uint8_t *buf, size_t len, uint8_t seed)
{
	for (size_t i = 0; i < len; i++) {
		buf[i] = (uint8_t)(seed + (uint8_t)i);
	}
	buf[0] = 9;   /* a plausible stored version byte */
}

/* ---- the accept list ------------------------------------------------------ */

static void t_src_ok_is_empty_at_v9(void)
{
	/* v9 resized interior arrays, so nothing earlier is a prefix of it. If this
	 * ever starts failing, someone added a length — check it is genuinely an
	 * append boundary and not just "the right size". */
	CHECK(profile_migrate_src_ok(0) == 0);
	CHECK(profile_migrate_src_ok(69) == 0);      /* v1 */
	CHECK(profile_migrate_src_ok(294) == 0);     /* v6 */
	CHECK(profile_migrate_src_ok(sizeof(struct profile)) == 0);
}

static void t_src_ok_rejects_v8(void)
{
	/* The specific dangerous one. v8's 528 bytes are the same SIZE shape as a
	 * prefix would be, but v9 moved the fields behind byte 118, so copying them
	 * forward yields a corrupt profile that still passes every length check. */
	CHECK(profile_migrate_src_ok(528) == 0);
}

/* ---- the widen mechanism -------------------------------------------------- */

static void t_apply_copies_prefix_and_zeroes_tail(void)
{
	uint8_t src[SYNTH_LEN];
	struct profile out;

	mk_src(src, sizeof(src), 0x10);
	memset(&out, 0xEE, sizeof(out));   /* poison: prove the tail is written */

	CHECK(profile_migrate_apply(src, sizeof(src), &out, NULL) == 0);

	/* prefix preserved byte for byte, except the version stamp at [0] */
	CHECK(memcmp((const uint8_t *)&out + 1, src + 1, sizeof(src) - 1) == 0);
	/* tail zeroed, since no fill callback was supplied */
	for (size_t i = sizeof(src); i < sizeof(struct profile); i++) {
		CHECK(((const uint8_t *)&out)[i] == 0);
	}
}

static void t_apply_stamps_version(void)
{
	uint8_t src[SYNTH_LEN];
	struct profile out;

	mk_src(src, sizeof(src), 0x20);
	src[0] = 9;                      /* older version in the stored bytes */
	memset(&out, 0, sizeof(out));

	CHECK(profile_migrate_apply(src, sizeof(src), &out, NULL) == 0);
	CHECK(out.version == PROFILE_VERSION);
}

static void t_apply_calls_fill_with_source_length(void)
{
	uint8_t src[SYNTH_LEN];
	struct profile out;

	fill_calls = 0; fill_saw_len = -1;
	mk_src(src, sizeof(src), 0x30);
	memset(&out, 0, sizeof(out));

	CHECK(profile_migrate_apply(src, sizeof(src), &out, tail_fill) == 0);
	CHECK(fill_calls == 1);
	CHECK(fill_saw_len == (int)SYNTH_LEN);
	/* the callback's pattern survived — i.e. the tail is DEFAULT-filled, not
	 * zero-filled, which is the whole point of the callback existing */
	for (size_t i = SYNTH_LEN; i < sizeof(struct profile); i++) {
		CHECK(((const uint8_t *)&out)[i] == 0xA5);
	}
}

static void t_version_stamp_beats_a_careless_fill(void)
{
	uint8_t src[SYNTH_LEN];
	struct profile out;

	mk_src(src, sizeof(src), 0x40);
	memset(&out, 0, sizeof(out));

	CHECK(profile_migrate_apply(src, sizeof(src), &out, bad_fill) == 0);
	/* bad_fill wrote 0x7F into version; the stamp runs after and must win, or a
	 * migrated profile fails profile_validate() and gets discarded */
	CHECK(out.version == PROFILE_VERSION);
}

static void t_apply_rejects_bad_arguments(void)
{
	uint8_t src[SYNTH_LEN];
	struct profile out;

	mk_src(src, sizeof(src), 0x50);
	CHECK(profile_migrate_apply(NULL, sizeof(src), &out, NULL) == -1);
	CHECK(profile_migrate_apply(src, sizeof(src), NULL, NULL) == -1);
	CHECK(profile_migrate_apply(src, 0, &out, NULL) == -1);
	/* longer than the struct would overrun *out */
	CHECK(profile_migrate_apply(src, sizeof(struct profile) + 1u, &out, NULL) == -1);
}

/* ---- the production entry point ------------------------------------------- */

static void t_migrate_passes_current_length_through(void)
{
	uint8_t src[sizeof(struct profile)];
	struct profile out;

	mk_src(src, sizeof(src), 0x60);
	src[0] = PROFILE_VERSION;
	memset(&out, 0xEE, sizeof(out));

	CHECK(profile_migrate(src, sizeof(src), &out, tail_fill) == 0);
	CHECK(memcmp(&out, src, sizeof(src)) == 0);
}

static void t_migrate_current_length_does_not_fill(void)
{
	uint8_t src[sizeof(struct profile)];
	struct profile out;

	fill_calls = 0;
	mk_src(src, sizeof(src), 0x70);
	src[0] = PROFILE_VERSION;

	CHECK(profile_migrate(src, sizeof(src), &out, tail_fill) == 0);
	/* An already-current profile has no missing tail. Running the fill over it
	 * would overwrite live settings with defaults. */
	CHECK(fill_calls == 0);
}

static void t_migrate_rejects_unlisted_length(void)
{
	uint8_t src[SYNTH_LEN];
	struct profile out;

	mk_src(src, sizeof(src), 0x80);
	/* SYNTH_LEN is not on the accept list, so the production path must refuse it
	 * and leave the caller to write a fresh default. */
	CHECK(profile_migrate(src, sizeof(src), &out, tail_fill) == -1);
	CHECK(profile_migrate(src, 528, &out, tail_fill) == -1);   /* v8 */
	CHECK(profile_migrate(src, 0, &out, tail_fill) == -1);
}

/* ---- restartability ------------------------------------------------------- */

static void t_sweep_is_restartable_over_a_mixed_store(void)
{
	/* NVS garbage-collects partway through a multi-slot rewrite, so a power loss
	 * leaves some slots migrated and some not. Model that: run the same sweep
	 * twice over a store where only some slots are current, and require the
	 * already-done ones to come through untouched. */
	enum { SLOTS = 16, DONE = 6 };
	uint8_t store[SLOTS][sizeof(struct profile)];
	size_t  len[SLOTS];
	struct profile out;

	for (int i = 0; i < SLOTS; i++) {
		mk_src(store[i], sizeof(struct profile), (uint8_t)(i + 1));
		if (i < DONE) {
			store[i][0] = PROFILE_VERSION;         /* already migrated */
			len[i] = sizeof(struct profile);
		} else {
			store[i][0] = 9;                        /* still old */
			len[i] = SYNTH_LEN;
		}
	}

	/* First pass: the already-current slots pass through; the rest are refused
	 * at v9 (nothing is append-compatible), which is the caller's cue to write a
	 * fresh default. Either way the sweep must not corrupt a done slot. */
	for (int pass = 0; pass < 2; pass++) {
		for (int i = 0; i < SLOTS; i++) {
			memset(&out, 0xEE, sizeof(out));
			int rc = profile_migrate(store[i], len[i], &out, tail_fill);
			if (i < DONE) {
				CHECK(rc == 0);
				CHECK(memcmp(&out, store[i], sizeof(struct profile)) == 0);
			} else {
				CHECK(rc == -1);
			}
		}
	}
}

int main(void)
{
	t_src_ok_is_empty_at_v9();
	t_src_ok_rejects_v8();
	t_apply_copies_prefix_and_zeroes_tail();
	t_apply_stamps_version();
	t_apply_calls_fill_with_source_length();
	t_version_stamp_beats_a_careless_fill();
	t_apply_rejects_bad_arguments();
	t_migrate_passes_current_length_through();
	t_migrate_current_length_does_not_fill();
	t_migrate_rejects_unlisted_length();
	t_sweep_is_restartable_over_a_mixed_store();
	printf("all profile_migrate tests passed (%d checks)\n", checks);
	return 0;
}
