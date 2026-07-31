#include "midi_delay.h"

#include <stddef.h>
#include <string.h>

static int pedal_index(uint8_t cc)
{
    switch (cc) {
    case 64: return 0;
    case 66: return 1;
    case 67: return 2;
    default: return -1;
    }
}

uint16_t midi_delay_styled_ticks(uint16_t straight_ticks, uint8_t style)
{
    uint32_t ticks = straight_ticks;
    if (style == MIDI_DELAY_STYLE_DOTTED) {
        ticks = ticks * 3u / 2u;
    } else if (style == MIDI_DELAY_STYLE_TRIPLET) {
        ticks = ticks * 2u / 3u;
    } else if (style != MIDI_DELAY_STYLE_STRAIGHT) {
        return 0;
    }
    return ticks <= UINT16_MAX ? (uint16_t)ticks : 0;
}

static bool tick_reached(uint32_t now, uint32_t due)
{
    return (int32_t)(now - due) >= 0;
}

static bool tick_before(uint32_t a, uint32_t b)
{
    return (int32_t)(a - b) < 0;
}

static void clear_pending(struct midi_delay *d)
{
    d->head = 0;
    d->count = 0;
}

/* Stable due-time insertion into the bounded ring. Repeat generations can be
 * earlier than a later input's first echo, so append-only FIFO order is not
 * sufficient once multi-repeat is enabled. */
static bool queue_insert(struct midi_delay *d,
                         const struct midi_delay_event *event)
{
    if (d->count >= MIDI_DELAY_QUEUE_CAP) {
        return false;
    }
    uint16_t pos = d->count;
    while (pos > 0) {
        uint16_t prev = (uint16_t)((d->head + pos - 1u) %
                                   MIDI_DELAY_QUEUE_CAP);
        if (!tick_before(event->due_tick, d->queue[prev].due_tick)) {
            break;
        }
        uint16_t dst = (uint16_t)((d->head + pos) %
                                  MIDI_DELAY_QUEUE_CAP);
        d->queue[dst] = d->queue[prev];
        pos--;
    }
    uint16_t dst = (uint16_t)((d->head + pos) % MIDI_DELAY_QUEUE_CAP);
    d->queue[dst] = *event;
    d->count++;
    return true;
}

static void write_release(struct midi_msg *out, int cap, int n,
                          uint8_t status, uint8_t d1, uint8_t d2)
{
    if (out && n < cap) {
        out[n].status = status;
        out[n].d1 = d1;
        out[n].d2 = d2;
        out[n].len = 3;
    }
}

static int release_active(struct midi_delay *d, struct midi_msg *out, int cap)
{
    int n = 0;
    uint8_t note_status = (uint8_t)(0x80u | d->output_channel);
    uint8_t cc_status = (uint8_t)(0xB0u | d->output_channel);
    static const uint8_t pedal_cc[3] = { 64, 66, 67 };

    for (int note = 0; note < 128; note++) {
        if (d->active_notes[note]) {
            write_release(out, cap, n, note_status, (uint8_t)note, 0);
            n++;
            d->active_notes[note] = 0;
        }
    }
    for (int p = 0; p < 3; p++) {
        if (d->pedal_output[p]) {
            write_release(out, cap, n, cc_status, pedal_cc[p], 0);
            n++;
        }
        d->pedal_output[p] = 0;
    }
    for (unsigned generation = 0; generation < MIDI_DELAY_MAX_REPEATS;
         generation++) {
        memset(d->pedal_value[generation], 0,
               sizeof(d->pedal_value[generation]));
    }
    return n;
}

static bool accepted_message(const struct midi_delay *d,
                             const struct midi_msg *in,
                             struct midi_msg *normalized)
{
    if (!in || (in->status & 0x80u) == 0 || in->status >= 0xF0u) {
        return false;
    }

    uint8_t kind = in->status & 0xF0u;
    uint8_t channel = in->status & 0x0Fu;
    if ((d->input_channel != MIDI_DELAY_CHANNEL_OMNI &&
         channel != d->input_channel) ||
        (d->input_channel == MIDI_DELAY_CHANNEL_OMNI &&
         channel == d->output_channel)) {
        return false;
    }

    if (kind == 0x90u) {
        if (in->len != 3 || in->d1 > 127 || in->d2 > 127) {
            return false;
        }
        *normalized = *in;
        normalized->status = (uint8_t)((in->d2 == 0 ? 0x80u : 0x90u) |
                                       d->output_channel);
        normalized->len = 3;
        return true;
    }
    if (kind == 0x80u) {
        if (in->len != 3 || in->d1 > 127 || in->d2 > 127) {
            return false;
        }
        *normalized = *in;
        normalized->status = (uint8_t)(0x80u | d->output_channel);
        normalized->len = 3;
        return true;
    }
    if ((kind == 0xA0u || kind == 0xE0u) &&
        in->len == 3 && in->d1 <= 127 && in->d2 <= 127) {
        *normalized = *in;
        normalized->status = (uint8_t)(kind | d->output_channel);
        normalized->len = 3;
        return true;
    }
    if (kind == 0xB0u && in->len == 3 &&
        in->d1 <= 119 && in->d2 <= 127) {
        *normalized = *in;
        normalized->status = (uint8_t)(0xB0u | d->output_channel);
        normalized->len = 3;
        return true;
    }
    if (kind == 0xD0u && in->len == 2 && in->d1 <= 127) {
        *normalized = *in;
        normalized->status = (uint8_t)(0xD0u | d->output_channel);
        normalized->d2 = 0;
        normalized->len = 2;
        return true;
    }
    return false;
}

static uint8_t decayed_velocity(uint8_t velocity, uint8_t decay,
                                uint8_t repeat_index)
{
    uint32_t value = velocity;
    for (uint8_t i = 0; i < repeat_index; i++) {
        value = (value * decay + 63u) / 127u;
    }
    return (uint8_t)value;
}

static bool transform_repeat(const struct midi_delay *d,
                             const struct midi_delay_event *event,
                             struct midi_msg *out)
{
    *out = event->msg;
    uint8_t kind = out->status & 0xF0u;

    if (kind == 0x90u && out->d2 != 0) {
        out->d2 = decayed_velocity(out->d2, d->velocity_decay,
                                   event->repeat_index);
        /* A zero-velocity generated repeat is silent, including its matching
         * note-off generation. */
        if (out->d2 == 0) {
            return false;
        }
    }

    if (kind == 0x80u || kind == 0x90u || kind == 0xA0u) {
        int note = (int)out->d1 +
                   (int)d->pitch_step * (int)event->repeat_index;
        if (note < 0 || note > 127) {
            return false;
        }
        out->d1 = (uint8_t)note;
    }
    return true;
}

static bool admit_transformed(struct midi_delay *d,
                              const struct midi_delay_event *event,
                              struct midi_msg *m)
{
    uint8_t kind = m->status & 0xF0u;
    if (kind == 0x90u && m->d2 != 0) {
        if (d->active_notes[m->d1] != UINT8_MAX) {
            d->active_notes[m->d1]++;
        }
        return true;
    } else if (kind == 0x80u || (kind == 0x90u && m->d2 == 0)) {
        if (d->active_notes[m->d1]) {
            d->active_notes[m->d1]--;
            /* MIDI 1.0 cannot identify overlapping instances of one pitch.
             * Hold the wire note until its last delay-owned reference ends. */
            return d->active_notes[m->d1] == 0;
        }
        /* Preserve unmatched source note-offs (including normalized note-on
         * velocity zero). They are harmless on the isolated output channel and
         * keep the delay faithful when it attaches mid-performance. */
        return true;
    } else if (kind == 0xB0u) {
        int p = pedal_index(m->d1);
        if (p >= 0) {
            uint8_t generation = event->repeat_index;
            d->pedal_value[generation][p] = m->d2;
            uint8_t aggregate = 0;
            for (unsigned r = 0; r < MIDI_DELAY_MAX_REPEATS; r++) {
                if (d->pedal_value[r][p] > aggregate) {
                    aggregate = d->pedal_value[r][p];
                }
            }
            if (aggregate == d->pedal_output[p]) {
                return false;
            }
            d->pedal_output[p] = aggregate;
            m->d2 = aggregate;
        }
    }
    return true;
}

void midi_delay_init(struct midi_delay *d, uint16_t delay_ticks,
                     uint8_t input_channel, uint8_t output_channel)
{
    if (!d) {
        return;
    }
    memset(d, 0, sizeof(*d));
    d->delay_ticks = delay_ticks;
    d->input_channel = input_channel == MIDI_DELAY_CHANNEL_OMNI
                     ? MIDI_DELAY_CHANNEL_OMNI
                     : (uint8_t)(input_channel & 0x0Fu);
    d->output_channel = (uint8_t)(output_channel & 0x0Fu);
    d->repeats = 1;
    d->velocity_decay = 127;
}

void midi_delay_start(struct midi_delay *d)
{
    if (d) {
        d->running = true;
    }
}

int midi_delay_stop(struct midi_delay *d, struct midi_msg *out, int cap)
{
    if (!d || cap < 0) {
        return MIDI_DELAY_ERR_ARGUMENT;
    }
    d->running = false;
    clear_pending(d);
    return release_active(d, out, cap);
}

int midi_delay_set_length(struct midi_delay *d, uint16_t delay_ticks,
                          struct midi_msg *out, int cap)
{
    if (!d || delay_ticks == 0 || cap < 0) {
        return MIDI_DELAY_ERR_ARGUMENT;
    }
    if (delay_ticks == d->delay_ticks) {
        return 0;
    }
    clear_pending(d);
    int n = release_active(d, out, cap);
    d->delay_ticks = delay_ticks;
    return n;
}

int midi_delay_set_character(struct midi_delay *d, uint8_t repeats,
                             uint8_t velocity_decay, int8_t pitch_step,
                             struct midi_msg *out, int cap)
{
    if (!d || repeats == 0 || repeats > MIDI_DELAY_MAX_REPEATS ||
        velocity_decay > 127 || pitch_step < -24 || pitch_step > 24 ||
        cap < 0) {
        return MIDI_DELAY_ERR_ARGUMENT;
    }
    if (d->repeats == repeats && d->velocity_decay == velocity_decay &&
        d->pitch_step == pitch_step) {
        return 0;
    }
    clear_pending(d);
    int n = release_active(d, out, cap);
    d->repeats = repeats;
    d->velocity_decay = velocity_decay;
    d->pitch_step = pitch_step;
    return n;
}

int midi_delay_set_channels(struct midi_delay *d, uint8_t input_channel,
                            uint8_t output_channel,
                            struct midi_msg *out, int cap)
{
    if (!d || (input_channel > 15 &&
               input_channel != MIDI_DELAY_CHANNEL_OMNI) ||
        output_channel > 15 || cap < 0) {
        return MIDI_DELAY_ERR_ARGUMENT;
    }
    if (d->input_channel == input_channel &&
        d->output_channel == output_channel) {
        return 0;
    }
    clear_pending(d);
    int n = release_active(d, out, cap);
    d->input_channel = input_channel;
    d->output_channel = output_channel;
    return n;
}

enum midi_delay_result midi_delay_input(struct midi_delay *d,
                                        uint32_t input_tick,
                                        const struct midi_msg *msg)
{
    if (!d || !msg) {
        return MIDI_DELAY_ERR_ARGUMENT;
    }
    if (!d->running) {
        return MIDI_DELAY_IGNORED;
    }

    struct midi_msg normalized;
    if (!accepted_message(d, msg, &normalized)) {
        return MIDI_DELAY_IGNORED;
    }
    if (d->count >= MIDI_DELAY_QUEUE_CAP) {
        return MIDI_DELAY_ERR_FULL;
    }

    struct midi_delay_event event = {
        .due_tick = input_tick + d->delay_ticks,
        .msg = normalized,
        .repeat_index = 0,
    };
    (void)queue_insert(d, &event); /* capacity was checked above */
    return MIDI_DELAY_OK;
}

int midi_delay_advance(struct midi_delay *d, uint32_t now_tick,
                       struct midi_msg *out, int cap)
{
    if (!d || cap < 0 || (cap > 0 && !out)) {
        return MIDI_DELAY_ERR_ARGUMENT;
    }
    if (!d->running) {
        return 0;
    }

    int n = 0;
    while (d->count && n < cap) {
        struct midi_delay_event event = d->queue[d->head];
        if (!tick_reached(now_tick, event.due_tick)) {
            break;
        }
        d->head = (uint16_t)((d->head + 1u) % MIDI_DELAY_QUEUE_CAP);
        d->count--;

        struct midi_msg transformed;
        if (transform_repeat(d, &event, &transformed) &&
            admit_transformed(d, &event, &transformed)) {
            out[n] = transformed;
            n++;
        }

        if ((uint8_t)(event.repeat_index + 1u) < d->repeats) {
            event.repeat_index++;
            event.due_tick += d->delay_ticks;
            (void)queue_insert(d, &event); /* pop above made one free slot */
        }
    }
    return n;
}

uint16_t midi_delay_pending(const struct midi_delay *d)
{
    return d ? d->count : 0;
}

bool midi_delay_running(const struct midi_delay *d)
{
    return d && d->running;
}
