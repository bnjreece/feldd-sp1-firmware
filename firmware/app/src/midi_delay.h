#ifndef MIDI_DELAY_H
#define MIDI_DELAY_H

/*
 * midi_delay.h - pure, bounded first-proof musical MIDI delay.
 *
 * The caller supplies monotonically advancing 96-PPQN ticks. While running,
 * accepted input is queued once at input_tick + delay_ticks. The module accepts
 * note on/off (including note-on velocity zero), poly/channel pressure,
 * controllers CC0..119, and pitch bend. It remaps accepted messages to one
 * configured output channel. Program change, channel-mode CC120..127, SysEx,
 * system-common, and realtime remain deliberately excluded.
 *
 * No Zephyr, hardware, allocation, output calls, or globals live here.
 */

#include <stdbool.h>
#include <stdint.h>

#include "mapping.h"

#define MIDI_DELAY_QUEUE_CAP       256u
#define MIDI_DELAY_RELEASE_CAP     131u /* 128 notes + CC64/66/67 */
#define MIDI_DELAY_CHANNEL_OMNI    0xFFu
#define MIDI_DELAY_MAX_REPEATS     8u
#define MIDI_DELAY_STYLE_COUNT     3u

enum midi_delay_style {
    MIDI_DELAY_STYLE_STRAIGHT = 0,
    MIDI_DELAY_STYLE_DOTTED = 1,
    MIDI_DELAY_STYLE_TRIPLET = 2,
};

enum midi_delay_result {
    MIDI_DELAY_OK = 0,
    MIDI_DELAY_IGNORED = 1,
    MIDI_DELAY_ERR_ARGUMENT = -1,
    MIDI_DELAY_ERR_FULL = -2,
};

struct midi_delay_event {
    uint32_t due_tick;
    struct midi_msg msg;
    uint8_t repeat_index;
};

struct midi_delay {
    struct midi_delay_event queue[MIDI_DELAY_QUEUE_CAP];
    uint16_t head;
    uint16_t count;
    uint16_t delay_ticks;
    uint8_t input_channel;       /* wire channel 0..15, or OMNI */
    uint8_t output_channel;      /* wire channel 0..15 */
    uint8_t repeats;             /* 1..MIDI_DELAY_MAX_REPEATS */
    uint8_t velocity_decay;      /* 0..127; 127 preserves each repeat */
    int8_t pitch_step;           /* semitones applied after each repeat */
    bool running;
    uint8_t active_notes[128];   /* emitted note-on reference counts */
    uint8_t pedal_value[MIDI_DELAY_MAX_REPEATS][3]; /* per-generation state */
    uint8_t pedal_output[3];     /* emitted aggregate CC64/66/67 values */
};

void midi_delay_init(struct midi_delay *d, uint16_t delay_ticks,
                     uint8_t input_channel, uint8_t output_channel);

/* Apply the selected rhythmic family to one straight-grid tick length. */
uint16_t midi_delay_styled_ticks(uint16_t straight_ticks, uint8_t style);

void midi_delay_start(struct midi_delay *d);

/*
 * Stop, discard every pending repeat, and fill out[] with releases for sounding
 * delay-owned notes/pedals. Returns the total release count required. When cap
 * is smaller than that count, only the first cap messages are written, but all
 * internal ownership is still cleared; runtime callers must pass
 * MIDI_DELAY_RELEASE_CAP and panic if they cannot.
 */
int midi_delay_stop(struct midi_delay *d, struct midi_msg *out, int cap);

/*
 * Change the delay interval. Like a hardware delay-time change, this cancels
 * pending repeats and releases already-sounding delay-owned state so an old
 * delayed note-off cannot be stranded. Running/stopped state is preserved.
 */
int midi_delay_set_length(struct midi_delay *d, uint16_t delay_ticks,
                          struct midi_msg *out, int cap);

/*
 * Change the repeat character. The first echo is unchanged; later echoes use
 * velocity_decay and pitch_step once per generation. Reconfiguration starts a
 * new safe epoch: pending events are cancelled and sounding delay-owned notes
 * and pedals are released, while the running/stopped state is preserved.
 */
int midi_delay_set_character(struct midi_delay *d, uint8_t repeats,
                             uint8_t velocity_decay, int8_t pitch_step,
                             struct midi_msg *out, int cap);

/*
 * Change accepted/output channels using the same safe-epoch behavior.
 * input_channel may be 0..15 or MIDI_DELAY_CHANNEL_OMNI. Omni mode rejects the
 * output channel, preventing a returned echo from recursively feeding itself.
 */
int midi_delay_set_channels(struct midi_delay *d, uint8_t input_channel,
                            uint8_t output_channel,
                            struct midi_msg *out, int cap);

/*
 * Queue one accepted input message. input_tick is an absolute 96-PPQN tick.
 * Returns OK, IGNORED (stopped, wrong channel, or unsupported message), or an
 * error. ERR_FULL leaves the existing queue untouched.
 */
enum midi_delay_result midi_delay_input(struct midi_delay *d,
                                        uint32_t input_tick,
                                        const struct midi_msg *msg);

/*
 * Emit due messages in FIFO order. Returns the number written. If more than cap
 * are due, call again with the same now_tick to drain the remainder.
 */
int midi_delay_advance(struct midi_delay *d, uint32_t now_tick,
                       struct midi_msg *out, int cap);

uint16_t midi_delay_pending(const struct midi_delay *d);
bool midi_delay_running(const struct midi_delay *d);

#endif /* MIDI_DELAY_H */
