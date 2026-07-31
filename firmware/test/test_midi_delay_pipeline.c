#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "../app/src/clockgen.h"
#include "../app/src/midi_delay.h"
#include "../app/src/midi_rt_ring.h"
#include "../app/src/usb_rt_parse.h"

static enum midi_delay_result feed_usb(struct midi_delay *delay, uint32_t tick,
                                       uint8_t cin, uint8_t status,
                                       uint8_t d1, uint8_t d2)
{
    uint8_t packet[4] = { cin, status, d1, d2 };
    uint8_t bytes[3];
    uint8_t len = usb_midi_extract_voice(packet, bytes);
    assert(len == 2 || len == 3);
    struct midi_msg msg = {
        .status = bytes[0],
        .d1 = bytes[1],
        .d2 = len == 3 ? bytes[2] : 0,
        .len = len,
    };
    return midi_delay_input(delay, tick, &msg);
}

static void enqueue_output(struct midi_rt_ring *ring, const struct midi_msg *msg)
{
    uint8_t bytes[3] = { msg->status, msg->d1, msg->d2 };
    assert(midi_rt_put_msg(ring, bytes, msg->len));
}

static void expect_wire3(struct midi_rt_ring *ring, uint8_t status,
                         uint8_t d1, uint8_t d2)
{
    uint8_t byte;
    assert(midi_rt_next(ring, &byte) && byte == status);
    assert(midi_rt_next(ring, &byte) && byte == d1);
    assert(midi_rt_next(ring, &byte) && byte == d2);
    assert(!midi_rt_next(ring, &byte));
}

static void test_usb_to_delayed_trs_note(void)
{
    struct midi_delay delay;
    struct midi_rt_ring ring;
    struct midi_msg out[4];

    midi_delay_init(&delay, 48, 0, 1);
    midi_rt_ring_init(&ring);
    midi_delay_start(&delay);

    assert(feed_usb(&delay, 100, 0x09, 0x90, 60, 101) == MIDI_DELAY_OK);
    assert(midi_delay_advance(&delay, 147, out, 4) == 0);
    assert(midi_delay_advance(&delay, 148, out, 4) == 1);
    enqueue_output(&ring, &out[0]);
    expect_wire3(&ring, 0x91, 60, 101);

    assert(feed_usb(&delay, 124, 0x08, 0x80, 60, 37) == MIDI_DELAY_OK);
    assert(midi_delay_advance(&delay, 172, out, 4) == 1);
    enqueue_output(&ring, &out[0]);
    expect_wire3(&ring, 0x81, 60, 37);
}

static void test_all_lengths_at_tempo_extremes(void)
{
    static const uint16_t bpm[] = { 40, 120, 240 };
    static const uint16_t lengths[] = {
        24, 48, 96, 192, 384, 768, 1536, 3072
    };

    for (unsigned b = 0; b < sizeof(bpm) / sizeof(bpm[0]); b++) {
        uint32_t subtick_us = (clockgen_tick_us(bpm[b]) + 2u) / 4u;
        assert(subtick_us > 0);
        for (unsigned l = 0; l < sizeof(lengths) / sizeof(lengths[0]); l++) {
            struct midi_delay delay;
            struct midi_msg out[1];
            midi_delay_init(&delay, lengths[l], 0, 1);
            midi_delay_start(&delay);
            assert(feed_usb(&delay, 500, 0x09, 0x90, 64, 88) ==
                   MIDI_DELAY_OK);
            assert(midi_delay_advance(&delay, 500u + lengths[l] - 1u,
                                      out, 1) == 0);
            assert(midi_delay_advance(&delay, 500u + lengths[l],
                                      out, 1) == 1);
            assert(out[0].status == 0x91 && out[0].d2 == 88);
            /* Verify the clock math stays representable for the longest delay. */
            uint64_t elapsed_us = (uint64_t)subtick_us * lengths[l];
            assert(elapsed_us > 0 && elapsed_us < UINT32_MAX);
        }
    }
}

static void test_feedback_channel_is_rejected(void)
{
    struct midi_delay delay;
    midi_delay_init(&delay, 24, 0, 1);
    midi_delay_start(&delay);

    /* A channel-2 echo returning from the host must not grow another repeat. */
    assert(feed_usb(&delay, 0, 0x09, 0x91, 60, 100) ==
           MIDI_DELAY_IGNORED);
    assert(midi_delay_pending(&delay) == 0);
}

static void test_disconnect_stop_releases_note_and_pedal(void)
{
    struct midi_delay delay;
    struct midi_msg out[4];
    struct midi_msg release[MIDI_DELAY_RELEASE_CAP];

    midi_delay_init(&delay, 24, 0, 1);
    midi_delay_start(&delay);
    assert(feed_usb(&delay, 0, 0x09, 0x90, 48, 90) == MIDI_DELAY_OK);
    assert(feed_usb(&delay, 0, 0x0B, 0xB0, 64, 73) == MIDI_DELAY_OK);
    assert(midi_delay_advance(&delay, 24, out, 4) == 2);

    int n = midi_delay_stop(&delay, release, MIDI_DELAY_RELEASE_CAP);
    assert(n == 2);
    assert(release[0].status == 0x81 && release[0].d1 == 48);
    assert(release[1].status == 0xB1 && release[1].d1 == 64 &&
           release[1].d2 == 0);
}

static void test_long_deterministic_performance(void)
{
    struct midi_delay delay;
    struct midi_rt_ring ring;
    struct midi_msg out[8];
    struct midi_msg release[MIDI_DELAY_RELEASE_CAP];
    uint32_t emitted = 0;

    midi_delay_init(&delay, 24, 0, 1);
    midi_rt_ring_init(&ring);
    midi_delay_start(&delay);

    for (uint32_t tick = 0; tick < 100000; tick++) {
        uint8_t note = (uint8_t)(36u + ((tick / 2u) % 64u));
        if ((tick & 1u) == 0) {
            assert(feed_usb(&delay, tick, 0x09, 0x90, note,
                            (uint8_t)(1u + tick % 127u)) == MIDI_DELAY_OK);
        } else {
            assert(feed_usb(&delay, tick, 0x08, 0x80, note, 0) ==
                   MIDI_DELAY_OK);
        }

        /* Exercise half-pedal values without allowing the queue to grow. */
        if (tick % 251u == 0) {
            assert(feed_usb(&delay, tick, 0x0B, 0xB0, 64,
                            (uint8_t)(tick % 128u)) == MIDI_DELAY_OK);
        }
        if (tick % 509u == 0) {
            assert(feed_usb(&delay, tick, 0x09, 0x91, note, 100) ==
                   MIDI_DELAY_IGNORED);
        }

        int n = midi_delay_advance(&delay, tick, out, 8);
        for (int i = 0; i < n; i++) {
            enqueue_output(&ring, &out[i]);
            for (uint8_t j = 0; j < out[i].len; j++) {
                uint8_t byte;
                assert(midi_rt_next(&ring, &byte));
            }
            emitted++;
        }
        assert(midi_delay_pending(&delay) < 32);
    }

    for (uint32_t tick = 100000; tick < 100024; tick++) {
        int n = midi_delay_advance(&delay, tick, out, 8);
        emitted += (uint32_t)n;
    }
    assert(emitted > 100000);
    int n = midi_delay_stop(&delay, release, MIDI_DELAY_RELEASE_CAP);
    assert(n >= 0 && n <= 3);
    assert(midi_delay_pending(&delay) == 0);
}

int main(void)
{
    test_usb_to_delayed_trs_note();
    test_all_lengths_at_tempo_extremes();
    test_feedback_channel_is_rejected();
    test_disconnect_stop_releases_note_and_pedal();
    test_long_deterministic_performance();
    puts("all midi_delay_pipeline tests passed");
    return 0;
}
