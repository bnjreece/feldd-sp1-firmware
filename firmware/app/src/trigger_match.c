/* trigger_match.c — pure note-match decision for the analog trigger output.
 * No Zephyr, no hardware: host-testable (see test_trigger_match.c). */
#include "trigger_match.h"

#define MIDI_STATUS_MASK  0xF0u
#define MIDI_CHANNEL_MASK 0x0Fu
#define MIDI_NOTE_ON      0x90u

int trigger_match_cfg_valid(const struct trigger_match_cfg *cfg)
{
	if (cfg == 0) {
		return 0;
	}
	if (cfg->note != TRIGGER_MATCH_ANY && cfg->note > 127u) {
		return 0;
	}
	if (cfg->channel != TRIGGER_MATCH_ANY && cfg->channel > 15u) {
		return 0;
	}
	return 1;
}

int trigger_match(const struct trigger_match_cfg *cfg, const uint8_t *bytes, uint8_t len)
{
	if (cfg == 0 || bytes == 0) {
		return 0;
	}

	/* A note message is always 3 bytes. Anything shorter cannot be one, and
	 * checking before touching bytes[1]/[2] keeps this safe against the 2-byte
	 * program-change / channel-pressure messages the same decoder emits. */
	if (len < 3u) {
		return 0;
	}

	const uint8_t status = bytes[0];

	/* Note-on only. 0x80 note-off is deliberately ignored: firing on both
	 * edges would advance the sequencer twice per note. */
	if ((status & MIDI_STATUS_MASK) != MIDI_NOTE_ON) {
		return 0;
	}

	/* Velocity 0 on a note-on IS a note-off. Sequencers emit these constantly;
	 * treating them as note-ons is the single easiest way to get double steps. */
	if (bytes[2] == 0u) {
		return 0;
	}

	if (cfg->channel != TRIGGER_MATCH_ANY &&
	    (status & MIDI_CHANNEL_MASK) != cfg->channel) {
		return 0;
	}

	if (cfg->note != TRIGGER_MATCH_ANY && bytes[1] != cfg->note) {
		return 0;
	}

	return 1;
}
