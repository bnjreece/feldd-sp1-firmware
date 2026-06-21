#ifndef CHORD6_H
#define CHORD6_H
#include <stdint.h>
#include "profile.h"

/* Pack an UNPACKED struct chord_def into the 6-byte stored chord6 (spec §2).
 * Explicit sets are clamped to CHORD6_MAX_EXPLICIT (5) stored notes. Mode/range/
 * root+quality map per the spec byte layout. Deterministic + total (no failure). */
void chord6_pack(const struct chord_def *in, struct chord6 *out);

/* Expand a stored chord6 back into a struct chord_def for chord_engine. Zeroes the
 * out first so unused fields are 0 (matching how chord_resolve reads them):
 *   explicit: mode=0, count, notes[0..count-1], quality=0 (mode-0 path)
 *   range:    range_start, range_count (>0), quality=0
 *   root+quality: mode=2, root, quality (>0 -> resolve_quality fires) */
void chord6_unpack(const struct chord6 *in, struct chord_def *out);

/* True iff a chord6 is "empty" (no notes / no range / no quality) -> plays nothing.
 * Used by the chord accessor to return NULL for an unconfigured button. */
int chord6_is_empty(const struct chord6 *c);

#endif
