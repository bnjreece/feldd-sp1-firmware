#include "midi_delay_runtime.h"

#include "clock_timer.h"
#include "midi_delay.h"
#include "midi_out.h"
#include "usb_midi1.h"

#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>

#define DELAY_THREAD_STACK 2048
#define DELAY_THREAD_PRIO  4
#define DELAY_EVENT_CAP    256
#define DELAY_EMIT_BATCH   16
#define DELAY_DEFAULT_INDEX 6u /* four bars */

enum delay_event_kind {
    DELAY_EV_TICK,
    DELAY_EV_MIDI,
    DELAY_EV_RUN,
    DELAY_EV_LENGTH,
    DELAY_EV_STYLE,
    DELAY_EV_CHARACTER,
    DELAY_EV_CHANNELS,
};

struct delay_runtime_event {
    uint32_t tick;
    struct midi_msg msg;
    uint8_t kind;
    uint8_t value;
};

static const uint16_t delay_ticks[] = {
    24, 48, 96, 192, 384, 768, 1536, 3072
};

K_MSGQ_DEFINE(delay_events, sizeof(struct delay_runtime_event),
              DELAY_EVENT_CAP, 4);
K_THREAD_STACK_DEFINE(delay_stack, DELAY_THREAD_STACK);
static struct k_thread delay_thread_data;
static struct midi_delay delay_core;
static atomic_t delay_tick;
static atomic_t requested_running;
static atomic_t requested_length;
static atomic_t requested_style;
static atomic_t requested_repeats;
static atomic_t requested_decay;
static atomic_t requested_pitch;
static atomic_t requested_input_channel;
static atomic_t requested_output_channel;
static atomic_t overflowed;
static bool initialized;

static bool post_event(const struct delay_runtime_event *event)
{
    if (k_msgq_put(&delay_events, event, K_NO_WAIT) == 0) {
        return true;
    }
    atomic_set(&overflowed, 1);
    return false;
}

static void emit_releases(void)
{
    struct midi_msg release[MIDI_DELAY_RELEASE_CAP];
    int n = midi_delay_stop(&delay_core, release, MIDI_DELAY_RELEASE_CAP);
    for (int i = 0; i < n && i < MIDI_DELAY_RELEASE_CAP; i++) {
        midi_out_send(&release[i], NULL);
    }
}

static void send_release_batch(struct midi_msg *release, int n)
{
    for (int i = 0; i < n && i < MIDI_DELAY_RELEASE_CAP; i++) {
        midi_out_send(&release[i], NULL);
    }
}

static void apply_length(void)
{
    struct midi_msg release[MIDI_DELAY_RELEASE_CAP];
    int n = midi_delay_set_length(&delay_core,
                midi_delay_styled_ticks(
                    delay_ticks[(uint8_t)atomic_get(&requested_length)],
                    (uint8_t)atomic_get(&requested_style)),
                release, MIDI_DELAY_RELEASE_CAP);
    send_release_batch(release, n);
}

static void apply_character(void)
{
    struct midi_msg release[MIDI_DELAY_RELEASE_CAP];
    int n = midi_delay_set_character(&delay_core,
                (uint8_t)atomic_get(&requested_repeats),
                (uint8_t)atomic_get(&requested_decay),
                (int8_t)atomic_get(&requested_pitch),
                release, MIDI_DELAY_RELEASE_CAP);
    send_release_batch(release, n);
}

static void apply_channels(void)
{
    struct midi_msg release[MIDI_DELAY_RELEASE_CAP];
    int n = midi_delay_set_channels(&delay_core,
                (uint8_t)atomic_get(&requested_input_channel),
                (uint8_t)atomic_get(&requested_output_channel),
                release, MIDI_DELAY_RELEASE_CAP);
    send_release_batch(release, n);
}

static void clock_subtick(void *ctx)
{
    ARG_UNUSED(ctx);
    struct delay_runtime_event event = {
        .kind = DELAY_EV_TICK,
        .tick = (uint32_t)atomic_inc(&delay_tick) + 1u,
    };
    (void)post_event(&event);
}

static void usb_voice_rx(const uint8_t *bytes, uint8_t len, void *ctx)
{
    ARG_UNUSED(ctx);
    if (!bytes || (len != 2 && len != 3)) {
        return;
    }
    struct delay_runtime_event event = {
        .kind = DELAY_EV_MIDI,
        .tick = (uint32_t)atomic_get(&delay_tick),
        .msg = {
            .status = bytes[0],
            .d1 = bytes[1],
            .d2 = len == 3 ? bytes[2] : 0,
            .len = len,
        },
    };
    (void)post_event(&event);
}

static void usb_link_change(bool connected, void *ctx)
{
    ARG_UNUSED(ctx);
    if (!connected) {
        (void)midi_delay_runtime_set_running(false);
    }
}

static void emit_due(uint32_t tick)
{
    struct midi_msg out[DELAY_EMIT_BATCH];
    int n;
    do {
        n = midi_delay_advance(&delay_core, tick, out, DELAY_EMIT_BATCH);
        for (int i = 0; i < n; i++) {
            midi_out_send(&out[i], NULL);
        }
    } while (n == DELAY_EMIT_BATCH);
}

static void delay_thread(void *a, void *b, void *c)
{
    ARG_UNUSED(a);
    ARG_UNUSED(b);
    ARG_UNUSED(c);

    for (;;) {
        struct delay_runtime_event event;
        k_msgq_get(&delay_events, &event, K_FOREVER);

        if (atomic_cas(&overflowed, 1, 0)) {
            emit_releases();
            atomic_set(&requested_running, 0);
        }

        switch (event.kind) {
        case DELAY_EV_TICK:
            emit_due(event.tick);
            break;
        case DELAY_EV_MIDI:
            (void)midi_delay_input(&delay_core, event.tick, &event.msg);
            break;
        case DELAY_EV_RUN:
            if (event.value) {
                midi_delay_start(&delay_core);
            } else {
                emit_releases();
            }
            break;
        case DELAY_EV_LENGTH:
        case DELAY_EV_STYLE:
            apply_length();
            break;
        case DELAY_EV_CHARACTER:
            apply_character();
            break;
        case DELAY_EV_CHANNELS:
            apply_channels();
            break;
        default:
            break;
        }
    }
}

int midi_delay_runtime_init(void)
{
    if (initialized) {
        return 0;
    }
    midi_delay_init(&delay_core, delay_ticks[DELAY_DEFAULT_INDEX], 0, 1);
    atomic_set(&delay_tick, 0);
    atomic_set(&requested_running, 0);
    atomic_set(&requested_length, DELAY_DEFAULT_INDEX);
    atomic_set(&requested_style, MIDI_DELAY_STYLE_STRAIGHT);
    atomic_set(&requested_repeats, 1);
    atomic_set(&requested_decay, 127);
    atomic_set(&requested_pitch, 0);
    atomic_set(&requested_input_channel, 0);
    atomic_set(&requested_output_channel, 1);
    atomic_set(&overflowed, 0);
    k_msgq_purge(&delay_events);
    k_thread_create(&delay_thread_data, delay_stack,
                    K_THREAD_STACK_SIZEOF(delay_stack), delay_thread,
                    NULL, NULL, NULL, DELAY_THREAD_PRIO, 0, K_NO_WAIT);
    k_thread_name_set(&delay_thread_data, "midi-delay");
    usb_midi1_set_voice_rx(usb_voice_rx, NULL);
    usb_midi1_set_link_callback(usb_link_change, NULL);
    clock_timer_set_subtick_listener(clock_subtick, NULL);
    initialized = true;
    return 0;
}

bool midi_delay_runtime_running(void)
{
    return atomic_get(&requested_running) != 0;
}

uint8_t midi_delay_runtime_length_index(void)
{
    return (uint8_t)atomic_get(&requested_length);
}

uint8_t midi_delay_runtime_style(void)
{
    return (uint8_t)atomic_get(&requested_style);
}

bool midi_delay_runtime_set_running(bool running)
{
    struct delay_runtime_event event = {
        .kind = DELAY_EV_RUN,
        .value = running ? 1u : 0u,
    };
    atomic_val_t old = atomic_set(&requested_running, running ? 1 : 0);
    if (!post_event(&event)) {
        atomic_set(&requested_running, old);
        return false;
    }
    return true;
}

bool midi_delay_runtime_toggle(void)
{
    return midi_delay_runtime_set_running(!midi_delay_runtime_running());
}

bool midi_delay_runtime_step_length(int direction)
{
    int old = (int)atomic_get(&requested_length);
    int next = old + (direction < 0 ? -1 : 1);
    if (next < 0) {
        next = 7;
    } else if (next > 7) {
        next = 0;
    }
    struct delay_runtime_event event = {
        .kind = DELAY_EV_LENGTH,
        .value = (uint8_t)next,
    };
    atomic_set(&requested_length, next);
    if (!post_event(&event)) {
        atomic_set(&requested_length, old);
        return false;
    }
    return true;
}

bool midi_delay_runtime_step_style(int direction)
{
    int old = (int)atomic_get(&requested_style);
    int next = old + (direction < 0 ? -1 : 1);
    if (next < 0) {
        next = MIDI_DELAY_STYLE_COUNT - 1;
    } else if (next >= MIDI_DELAY_STYLE_COUNT) {
        next = 0;
    }
    struct delay_runtime_event event = {
        .kind = DELAY_EV_STYLE,
        .value = (uint8_t)next,
    };
    atomic_set(&requested_style, next);
    if (!post_event(&event)) {
        atomic_set(&requested_style, old);
        return false;
    }
    return true;
}

bool midi_delay_runtime_set_repeats(uint8_t repeats)
{
    if (repeats == 0 || repeats > MIDI_DELAY_MAX_REPEATS) {
        return false;
    }
    atomic_val_t old = atomic_get(&requested_repeats);
    if ((uint8_t)old == repeats) {
        return true;
    }
    atomic_set(&requested_repeats, repeats);
    struct delay_runtime_event event = { .kind = DELAY_EV_CHARACTER };
    if (!post_event(&event)) {
        atomic_set(&requested_repeats, old);
        return false;
    }
    return true;
}

bool midi_delay_runtime_set_velocity_decay(uint8_t decay)
{
    atomic_val_t old = atomic_get(&requested_decay);
    if ((uint8_t)old == decay) {
        return true;
    }
    atomic_set(&requested_decay, decay);
    struct delay_runtime_event event = { .kind = DELAY_EV_CHARACTER };
    if (!post_event(&event)) {
        atomic_set(&requested_decay, old);
        return false;
    }
    return true;
}

bool midi_delay_runtime_set_pitch_step(int8_t semitones)
{
    if (semitones < -24 || semitones > 24) {
        return false;
    }
    atomic_val_t old = atomic_get(&requested_pitch);
    if ((int8_t)old == semitones) {
        return true;
    }
    atomic_set(&requested_pitch, semitones);
    struct delay_runtime_event event = { .kind = DELAY_EV_CHARACTER };
    if (!post_event(&event)) {
        atomic_set(&requested_pitch, old);
        return false;
    }
    return true;
}

bool midi_delay_runtime_set_channels(uint8_t input_channel,
                                     uint8_t output_channel)
{
    if ((input_channel > 15 && input_channel != MIDI_DELAY_CHANNEL_OMNI) ||
        output_channel > 15) {
        return false;
    }
    atomic_val_t old_in = atomic_get(&requested_input_channel);
    atomic_val_t old_out = atomic_get(&requested_output_channel);
    if ((uint8_t)old_in == input_channel &&
        (uint8_t)old_out == output_channel) {
        return true;
    }
    atomic_set(&requested_input_channel, input_channel);
    atomic_set(&requested_output_channel, output_channel);
    struct delay_runtime_event event = { .kind = DELAY_EV_CHANNELS };
    if (!post_event(&event)) {
        atomic_set(&requested_input_channel, old_in);
        atomic_set(&requested_output_channel, old_out);
        return false;
    }
    return true;
}
