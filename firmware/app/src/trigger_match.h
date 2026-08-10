#ifndef TRIGGER_MATCH_H
#define TRIGGER_MATCH_H
#include <stdint.h>

/*
 * trigger_match.h — the PURE decision: should this MIDI message fire a trigger?
 *
 * Split out from trigger_out.c for the same reason mapping.c, chord6.c, dial.c
 * and friends are split out: no Zephyr dependency, so it compiles and is proven
 * on the host (firmware/test/) without flashing anything. All the fiddly MIDI
 * semantics live here where they can be tested exhaustively; trigger_out.c keeps
 * only the pin and the timer.
 */

#define TRIGGER_MATCH_ANY 0xFFu   /* wildcard: any note / omni channel */

struct trigger_match_cfg {
	uint8_t note;      /* 0..127, or TRIGGER_MATCH_ANY */
	uint8_t channel;   /* 0..15,  or TRIGGER_MATCH_ANY */
};

/*
 * Returns 1 if `bytes` (a decoded channel-voice message: status, d1, [d2], as
 * produced by usb_midi_extract_voice) should fire the trigger, else 0.
 *
 * Fires on NOTE-ON ONLY. Three things this gets right that a naive status-nibble
 * check does not:
 *
 *   - Note-on with velocity 0 is a note-OFF by MIDI convention, and many
 *     sequencers emit note-offs that way. It must not fire, or every note fires
 *     twice and the sequencer advances two steps per note.
 *   - 0x80 note-off never fires, for the same reason.
 *   - Running status is not handled here because the USB-MIDI 1.0 packet format
 *     already delivers each message with an explicit status byte.
 *
 * A short or malformed message (len < 3 for a note message) returns 0 rather
 * than reading past the buffer.
 */
int trigger_match(const struct trigger_match_cfg *cfg, const uint8_t *bytes, uint8_t len);

/* Range check for a configuration, so the caller can reject bad values before
 * storing them. Returns 1 if usable. */
int trigger_match_cfg_valid(const struct trigger_match_cfg *cfg);

#endif /* TRIGGER_MATCH_H */
