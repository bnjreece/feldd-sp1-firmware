#ifndef PROFILE_MIGRATE_H
#define PROFILE_MIGRATE_H
/*
 * profile_migrate.h — widen an older stored profile into the current struct.
 *
 * WHY THIS EXISTS. The profile format has two readers, and they have never been
 * equally forgiving:
 *
 *   - the WIRE path (profile_from_b64) has upgraded older blobs since v2, by
 *     decoding a shorter prefix and completing it with profile_fill_missing().
 *   - the NVS path (librarian_init) has no equivalent. A stored profile whose
 *     version does not match PROFILE_VERSION goes to seed_defaults(), i.e. every
 *     one of the user's profiles is replaced with factory defaults.
 *
 * This header is the missing half: the pure logic to take the bytes a previous
 * version stored and produce a valid current profile from them. Pure and
 * freestanding — no Zephyr, no flash, no NVS — following seed_cadence.h and
 * lib_header.h, so the host test drives the REAL functions rather than a
 * reimplementation.
 *
 * WHAT IT DOES NOT DO. Only an APPEND is migratable this way. If a version
 * changes the layout of existing fields rather than adding to the end, the older
 * bytes are not a prefix of the newer struct and copying them forward produces
 * garbage that still passes a length check. v9 is exactly such a version
 * (NUM_LAYERS 4 -> 8 resized interior arrays), which is why the accept list in
 * profile_migrate_src_ok() is EMPTY today. See the comment there before adding
 * anything to it.
 */
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "profile.h"

/* Injected tail completion, mirroring how seed_cadence.h injects its side
 * effects so the loop itself stays pure and host-testable. Called with the
 * struct already carrying the old bytes at the front and zeroes behind them;
 * `src_len` says which version produced those bytes, so the callback can apply
 * that version's semantics. In firmware this is profile.c's profile_fill_missing().
 *
 * A fill callback is REQUIRED to write real defaults rather than leaving the
 * zeroes: a zero byte is a legal-looking value for most fields, so a zero-filled
 * tail produces a profile that validates and misbehaves, which is far worse than
 * one that fails loudly. */
typedef void (*profile_fill_fn)(struct profile *out, int src_len);

/* Is `len` a stored length this build knows how to widen?
 *
 * ACCEPT ONLY TRUE PREFIXES OF THE CURRENT STRUCT. A length is safe here only if
 * every byte that version wrote means the same thing at the same offset today —
 * that is, if every version since then APPENDED. Adding a length from a version
 * that moved or resized existing fields turns a plausible input into a silently
 * corrupt profile, because the bytes copy cleanly and the length check passes.
 *
 * EMPTY AT v9, and that is not an oversight: v9 resized the interior layer/ext/
 * chord arrays (NUM_LAYERS 4 -> 8) rather than appending, so NO earlier version
 * is a prefix of it. v8's 528 bytes in particular must NEVER be listed here.
 * A future version that appends adds its predecessor's wire length. */
static inline int profile_migrate_src_ok(size_t len)
{
	/* v9's 1038 bytes. v10 APPENDED to them without touching a single existing
	 * field, so every v9 byte still means the same thing at the same offset and
	 * widening is safe. This is the first and only entry.
	 *
	 * v8's 528 bytes are NOT eligible and must never be added: v9 resized the
	 * interior layer/ext/chord arrays, so a v8 image is not a prefix of v9, and
	 * copying it forward would produce a corrupt profile from a plausible input. */
	if (len == 1038u) {
		return 1;
	}
	(void)len;
	return 0;
}

/* Widen `src_len` stored bytes into *out, assuming the length has ALREADY been
 * accepted (by profile_migrate_src_ok, or by a test). Separated from the policy
 * above so the mechanism can be exercised on lengths this build does not accept
 * in production — which at v9 is every length, since nothing is append-
 * compatible yet.
 *
 * Copies the prefix, zeroes the remainder, lets `fill` apply the old version's
 * semantics, then stamps the version LAST.
 *
 * Stamping last is deliberate: profile_validate() rejects any profile whose
 * version byte is not PROFILE_VERSION, so a migrated profile that is validated
 * before being stamped fails and is discarded — a wipe by a slower route.
 * Stamping after `fill` also stops a careless callback from overwriting it.
 *
 * Returns 0 on success, -1 if the arguments cannot produce a valid profile. */
static inline int profile_migrate_apply(const void *src, size_t src_len,
					struct profile *out, profile_fill_fn fill)
{
	if (src == NULL || out == NULL) {
		return -1;
	}
	if (src_len == 0u || src_len > sizeof(struct profile)) {
		return -1;
	}

	memcpy(out, src, src_len);
	memset((uint8_t *)out + src_len, 0, sizeof(struct profile) - src_len);

	if (fill != NULL) {
		fill(out, (int)src_len);
	}

	out->version = PROFILE_VERSION;
	return 0;
}

/* The production entry point: decide, then widen.
 *
 *   src_len == sizeof(struct profile) -> already current; copy verbatim, 0
 *   src_len accepted by _src_ok        -> widen + fill + stamp, 0
 *   anything else                      -> -1, and the CALLER must fall back to
 *                                         writing a fresh default profile
 *
 * The "already current" case exists so a caller can run this over every stored
 * slot without tracking which ones it has already done. That makes a migration
 * sweep restartable: NVS garbage-collects partway through a multi-slot rewrite,
 * so a power loss leaves a MIXED store, and the next boot must tolerate finding
 * slots in both states. nvs_read() reports the stored record's length, which is
 * exactly the discriminator this function wants. */
static inline int profile_migrate(const void *src, size_t src_len,
				  struct profile *out, profile_fill_fn fill)
{
	if (src_len == sizeof(struct profile)) {
		if (src == NULL || out == NULL) {
			return -1;
		}
		memcpy(out, src, sizeof(struct profile));
		return 0;
	}
	if (!profile_migrate_src_ok(src_len)) {
		return -1;
	}
	return profile_migrate_apply(src, src_len, out, fill);
}

#endif /* PROFILE_MIGRATE_H */
